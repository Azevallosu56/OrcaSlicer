#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

struct QidiFilamentEntry
{
    std::string orca_id;
    std::string qidi_id;
    std::string name;
    std::string type;
    std::string vendor;
    std::string firmware_vendor;
    std::string firmware_filament;
    int         nozzle_temp_min { 190 };
    int         nozzle_temp_max { 240 };
};

class QidiPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Filament ID boundary translation (QF* <-> OF*)
    std::string to_orca_filament_id(const std::string& printer_id) const override;
    std::string from_orca_filament_id(const std::string& orca_id) const override;

    // Override filament sync (Qidi-specific implementation: QIDI Box + legacy fallback)
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Print job execution with pre-print QIDI Box / leveling / Polar Cooler / timelapse setup
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;

    // Native Device tab camera & RFID support
    CameraStreamMode get_camera_stream_mode() const override;
    std::string      get_camera_url() const override;
    int              command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int extruder_id, bool is_sub_slot) override;

    // Testable static helpers
    // Builds the SAVE_VARIABLE lines that make each T<n> macro load the Box slot picked for filament n
    static std::string    build_tool_slot_script(const std::string& ams_mapping);
    // Builds the complete pre-print Klipper G-code script (enable_box, value_t*, G31/G32, enable_polar_cooler)
    static std::string    build_pre_print_script(const PrintParams& params, const std::set<std::string>& available_objects);
    // Builds Klipper UPDATE_FILAMENT_INFORMATION G-code for editing a QIDI Box slot or external spool (slot 16)
    static std::string    build_filament_update_script(int slot_index,
                                                       const std::string& color_hex,
                                                       const std::string& vendor,
                                                       const std::string& filament_str);
    // Builds the body of Moonraker's print start for an uploaded file, naming the plate to print when it is a 3MF
    static nlohmann::json print_start_request(const PrintParams& params, const std::string& filename);
    // Parses QIDI Moonraker status (multi_color_controller / box_extras / save_variables) into Orca push_status ams & vt_tray JSON
    static bool           parse_qidi_ams_status(const nlohmann::json& status,
                                                nlohmann::json&       out_ams,
                                                nlohmann::json&       out_vt_tray);

    // Catalog lookup helpers
    static std::string translate_qidi_to_orca_id(const std::string& qidi_id);
    static std::string translate_orca_to_qidi_id(const std::string& orca_id);
    static bool        lookup_filament_by_orca_id(const std::string& orca_id, QidiFilamentEntry& out_entry);
    static bool        lookup_filament_by_qidi_id(const std::string& qidi_id, QidiFilamentEntry& out_entry);

protected:
    bool                     print_uploaded_file(const PrintParams& params, const std::string& filename) override;
    std::string              ams_unit_name(int ams_id) const override { return "Qidi Box " + std::to_string(ams_id + 1); }
    std::vector<std::string> extra_subscription_objects(const std::set<std::string>& objects) const override;
    void                     augment_print_payload_locked(const nlohmann::json&        status,
                                                          const std::set<std::string>& objects,
                                                          nlohmann::json&              print_payload) const override;
    int                      handle_request(const std::string& dev_id, const std::string& json_str) override;

private:
    struct QidiFilamentDict
    {
        std::map<int, std::string> colors;
        std::map<int, std::string> filaments;
    };

    bool        fetch_slot_info(const std::string&        base_url,
                                const std::string&        api_key,
                                const QidiFilamentDict&   dict,
                                const std::string&        series_id,
                                std::vector<AmsTrayData>& trays,
                                int&                      box_count,
                                nlohmann::json&           raw_status,
                                std::string&              error);
    bool        fetch_filament_dict(const std::string& base_url, const std::string& api_key, QidiFilamentDict& dict, std::string& error) const;
    bool        set_timelapse_enabled(bool enabled) const;
    int         current_loaded_qidi_slot() const;
    static std::string normalize_filament_type(const std::string& filament_type);
    std::string infer_series_id(const std::string& model_id, const std::string& dev_name);
    std::string normalize_model_key(std::string value);

    // Static helpers
    static void        parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result);
    static void        parse_filament_sections(const std::string& content, std::map<int, std::string>& result);
    static std::string map_filament_type_to_setting_id(const std::string& filament_type);
    static std::string sanitize_hex_rgb6(const std::string& raw_color);
    static std::string sanitize_single_quoted(const std::string& value);
};

} // namespace Slic3r

#endif
