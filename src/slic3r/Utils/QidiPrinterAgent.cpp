#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

struct QidiFilamentCatalog
{
    std::string                                         loaded_path;
    std::unordered_map<std::string, QidiFilamentEntry>  by_orca_id;
    std::unordered_map<std::string, QidiFilamentEntry>  by_qidi_id;
};

QidiFilamentCatalog load_qidi_filament_catalog()
{
    static QidiFilamentCatalog catalog;
    static std::mutex          mutex;
    std::lock_guard<std::mutex> lock(mutex);

    const std::string path = resources_dir() + "/printers/qidi_filament_ids.json";
    if (catalog.loaded_path == path && !catalog.by_orca_id.empty()) {
        return catalog;
    }

    boost::nowide::ifstream file(path);
    if (!file.is_open()) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: could not open " << path;
        return catalog;
    }
    const auto root = nlohmann::json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.contains("filaments") || !root["filaments"].is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent: invalid JSON in " << path;
        return catalog;
    }

    QidiFilamentCatalog loaded;
    loaded.loaded_path = path;
    for (const auto& [orca_id, item] : root["filaments"].items()) {
        if (!item.is_object())
            continue;
        QidiFilamentEntry entry;
        entry.orca_id           = orca_id;
        entry.qidi_id           = item.value("qidi_id", std::string{});
        entry.name              = item.value("name", std::string{});
        entry.type              = item.value("type", std::string{});
        entry.vendor            = item.value("vendor", std::string{});
        entry.firmware_vendor   = item.value("firmware_vendor", std::string{"Generic"});
        entry.firmware_filament = item.value("firmware_filament", entry.type);
        entry.nozzle_temp_min   = item.value("nozzle_temp_min", 190);
        entry.nozzle_temp_max   = item.value("nozzle_temp_max", 240);
        loaded.by_orca_id.emplace(entry.orca_id, entry);
        if (!entry.qidi_id.empty()) {
            loaded.by_qidi_id.emplace(entry.qidi_id, entry);
        }
    }
    catalog = std::move(loaded);
    return catalog;
}

bool has_visible_base_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && filaments.get_preset_base(p) == &p
            && p.filament_id == filament_id)
            return true;
    }
    return false;
}

std::string to_rgba8_hex(const std::string& raw)
{
    std::string hex;
    hex.reserve(8);
    for (unsigned char c : raw) {
        if (std::isxdigit(c))
            hex.push_back(static_cast<char>(std::toupper(c)));
    }
    if (hex.size() == 6)
        return hex + "FF";
    if (hex.size() == 8)
        return hex;
    return "FFFFFFFF";
}

std::string extract_color_rgba8(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key))
        return "FFFFFFFF";
    const auto& val = j[key];
    if (val.is_string())
        return to_rgba8_hex(val.get<std::string>());
    if (val.is_array() && !val.empty() && val[0].is_string())
        return to_rgba8_hex(val[0].get<std::string>());
    return "FFFFFFFF";
}

int parse_slot_string(const std::string& slot_str)
{
    if (slot_str.size() > 4 && slot_str.rfind("slot", 0) == 0) {
        try {
            return std::stoi(slot_str.substr(4));
        } catch (...) {}
    }
    return -1;
}

std::string humidity_percent_to_level(int pct)
{
    if (pct <= 0)
        return "0";
    if (pct <= 20)
        return "5";
    if (pct <= 35)
        return "4";
    if (pct <= 50)
        return "3";
    if (pct <= 65)
        return "2";
    return "1";
}

std::string format_hex_bits(uint32_t mask)
{
    std::ostringstream ss;
    ss << std::hex << mask;
    return ss.str();
}

std::string fallback_orca_id_for_type(const std::string& tray_type)
{
    const std::string upper = boost::to_upper_copy(boost::trim_copy(tray_type));
    if (upper.find("PLA") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFA90");
    if (upper.find("PETG") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFG90");
    if (upper.find("ABS") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFB91");
    if (upper.find("ASA") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFB01");
    if (upper.find("TPU") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFU91");
    if (upper.find("PC") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFC90");
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return QidiPrinterAgent::translate_qidi_to_orca_id("QFN04");
    if (wxTheApp != nullptr && GUI::wxGetApp().preset_bundle != nullptr) {
        const std::string id = GUI::wxGetApp().preset_bundle->filaments.filament_id_by_type(tray_type);
        if (!id.empty())
            return id;
    }
    return MoonrakerPrinterAgent::map_filament_type_to_generic_id(tray_type);
}

bool has_mapped_box_slot(const PrintParams& params)
{
    const auto mapping = nlohmann::json::parse(params.ams_mapping, nullptr, false);
    if (mapping.is_array()) {
        for (const auto& v : mapping) {
            if (v.is_number_integer()) {
                const int s = v.get<int>();
                if (s >= 0 && s < 16)
                    return true;
            }
        }
    }
    return false;
}

} // anonymous namespace

const std::string QidiPrinterAgent_VERSION = "0.2.0";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{QIDI_PRINTER_AGENT_ID, "Qidi", QidiPrinterAgent_VERSION, "Qidi printer agent"};
}

std::string QidiPrinterAgent::translate_qidi_to_orca_id(const std::string& qidi_id)
{
    if (qidi_id.empty())
        return {};
    const auto cat = load_qidi_filament_catalog();
    if (cat.by_orca_id.find(qidi_id) != cat.by_orca_id.end())
        return qidi_id;
    const auto it = cat.by_qidi_id.find(qidi_id);
    if (it != cat.by_qidi_id.end())
        return it->second.orca_id;
    return qidi_id;
}

std::string QidiPrinterAgent::translate_orca_to_qidi_id(const std::string& orca_id)
{
    if (orca_id.empty())
        return {};
    const auto cat = load_qidi_filament_catalog();
    if (cat.by_qidi_id.find(orca_id) != cat.by_qidi_id.end())
        return orca_id;
    const auto it = cat.by_orca_id.find(orca_id);
    if (it != cat.by_orca_id.end())
        return it->second.qidi_id;
    return orca_id;
}

bool QidiPrinterAgent::lookup_filament_by_orca_id(const std::string& orca_id, QidiFilamentEntry& out_entry)
{
    const auto cat = load_qidi_filament_catalog();
    const auto it  = cat.by_orca_id.find(orca_id);
    if (it == cat.by_orca_id.end())
        return false;
    out_entry = it->second;
    return true;
}

bool QidiPrinterAgent::lookup_filament_by_qidi_id(const std::string& qidi_id, QidiFilamentEntry& out_entry)
{
    const auto cat = load_qidi_filament_catalog();
    const auto it  = cat.by_qidi_id.find(qidi_id);
    if (it == cat.by_qidi_id.end())
        return false;
    out_entry = it->second;
    return true;
}

std::string QidiPrinterAgent::to_orca_filament_id(const std::string& printer_id) const
{
    return translate_qidi_to_orca_id(printer_id);
}

std::string QidiPrinterAgent::from_orca_filament_id(const std::string& orca_id) const
{
    return translate_orca_to_qidi_id(orca_id);
}

CameraStreamMode QidiPrinterAgent::get_camera_stream_mode() const
{
    if (device_info.dev_ip.empty() && device_info.base_url.empty())
        return CameraStreamMode::none;
    return CameraStreamMode::http_snapshot;
}

std::string QidiPrinterAgent::get_camera_url() const
{
    if (device_info.base_url.empty() && device_info.dev_ip.empty())
        return {};
    // Moonraker's raw API port (:7125) does not proxy mjpg-streamer; nginx on :80 and :10088 serves /webcam/?action=snapshot
    if (!device_info.base_url.empty() && device_info.base_url.find(":7125") == std::string::npos) {
        return join_url(device_info.base_url, "/webcam/?action=snapshot");
    }
    if (!device_info.dev_ip.empty()) {
        std::string port;
        const std::string host = Http::get_host_from_url(device_info.dev_ip, &port);
        if (!host.empty())
            return "http://" + host + "/webcam/?action=snapshot";
    }
    return {};
}

int QidiPrinterAgent::command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int /*extruder_id*/, bool /*is_sub_slot*/)
{
    const int global_slot = ams_id * 4 + slot_id;
    if (global_slot < 0 || global_slot >= 16)
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    send_gcode_async(dev_id, "READ_RFID SLOT=" + std::to_string(global_slot));
    return BAMBU_NETWORK_SUCCESS;
}

std::vector<std::string> QidiPrinterAgent::extra_subscription_objects(const std::set<std::string>& objects) const
{
    std::vector<std::string> extras;
    static const char* const candidates[] = {
        "save_variables",
        "multi_color_controller",
        "box_extras",
        "box485",
        "heater_generic chamber",
        "fan_generic cooling_fan",
        "fan_generic auxiliary_cooling_fan",
        "fan_generic chamber_circulation_fan",
        "output_pin caselight",
        "output_pin polar_cooler",
    };
    for (const char* name : candidates) {
        if (objects.count(name) != 0)
            extras.emplace_back(name);
    }
    for (int i = 0; i < 16; ++i) {
        const std::string stepper = "box_stepper slot" + std::to_string(i);
        if (objects.count(stepper) != 0)
            extras.push_back(stepper);
    }
    return extras;
}

bool QidiPrinterAgent::parse_qidi_ams_status(const nlohmann::json& status,
                                             nlohmann::json&       out_ams,
                                             nlohmann::json&       out_vt_tray)
{
    out_ams     = nlohmann::json();
    out_vt_tray = nlohmann::json();

    const nlohmann::json* mcc = (status.contains("multi_color_controller") && status["multi_color_controller"].is_object())
                                    ? &status["multi_color_controller"]
                                    : nullptr;
    const nlohmann::json* bex = (status.contains("box_extras") && status["box_extras"].is_object())
                                    ? &status["box_extras"]
                                    : nullptr;
    const nlohmann::json* sv_vars = (status.contains("save_variables") && status["save_variables"].is_object() &&
                                     status["save_variables"].contains("variables") &&
                                     status["save_variables"]["variables"].is_object())
                                        ? &status["save_variables"]["variables"]
                                        : nullptr;

    if (!mcc && !bex && !sv_vars)
        return false;

    // 1. Determine active / loaded slot ("slot0".."slot15" -> "0".."15", "slot16" -> "254", else "255")
    std::string loaded_slot_str;
    if (mcc && mcc->contains("box") && (*mcc)["box"].is_object()) {
        loaded_slot_str = (*mcc)["box"].value("loaded_slot", (*mcc)["box"].value("last_load_slot", std::string{}));
    }
    if (loaded_slot_str.empty() && bex) {
        loaded_slot_str = bex->value("loaded_slot", bex->value("last_load_slot", std::string{}));
    }
    if (loaded_slot_str.empty() && sv_vars) {
        loaded_slot_str = sv_vars->value("last_load_slot", std::string{});
    }
    const int   loaded_slot_idx = parse_slot_string(loaded_slot_str);
    std::string tray_now_str    = "255";
    if (loaded_slot_idx >= 0 && loaded_slot_idx < 16)
        tray_now_str = std::to_string(loaded_slot_idx);
    else if (loaded_slot_idx == 16)
        tray_now_str = "254";

    // 2. Parse External Spool (slot16 -> vt_tray)
    if (mcc && mcc->contains("extra") && (*mcc)["extra"].is_object() &&
        (*mcc)["extra"].contains("filament_info") && (*mcc)["extra"]["filament_info"].is_object()) {
        const auto& ext = (*mcc)["extra"]["filament_info"];
        std::string qfid = ext.value("filament_id", std::string{});
        std::string ftype = normalize_filament_type(ext.value("filament_type", ext.value("type_str", std::string{"PLA"})));
        std::string orca_id = translate_qidi_to_orca_id(qfid);
        if (orca_id.empty() || orca_id == qfid)
            orca_id = fallback_orca_id_for_type(ftype);

        QidiFilamentEntry entry;
        const bool has_entry = lookup_filament_by_orca_id(orca_id, entry);
        const int  min_temp  = ext.value("min_temp", has_entry ? entry.nozzle_temp_min : 190);
        const int  max_temp  = ext.value("max_temp", has_entry ? entry.nozzle_temp_max : 240);
        const std::string rgba = extract_color_rgba8(ext, "color_rgb_str");

        out_vt_tray = nlohmann::json::object();
        out_vt_tray["id"]              = "254";
        out_vt_tray["tray_info_idx"]   = orca_id;
        out_vt_tray["tray_type"]       = ftype;
        out_vt_tray["tray_color"]      = rgba;
        out_vt_tray["cols"]            = nlohmann::json::array({rgba});
        out_vt_tray["ctype"]           = 0;
        out_vt_tray["nozzle_temp_min"] = std::to_string(min_temp);
        out_vt_tray["nozzle_temp_max"] = std::to_string(max_temp);
    }

    // 3. Determine QIDI Box count (0..4)
    int box_count = 0;
    if (mcc && mcc->contains("box_count") && (*mcc)["box_count"].is_number_integer()) {
        box_count = (*mcc)["box_count"].get<int>();
    } else if (mcc && mcc->contains("box") && (*mcc)["box"].is_object() && (*mcc)["box"].contains("box_count") &&
               (*mcc)["box"]["box_count"].is_number_integer()) {
        box_count = (*mcc)["box"]["box_count"].get<int>();
    } else if (sv_vars && sv_vars->contains("box_count") && (*sv_vars)["box_count"].is_number_integer()) {
        box_count = (*sv_vars)["box_count"].get<int>();
    } else if (bex && bex->contains("boxes") && (*bex)["boxes"].is_array()) {
        for (const auto& b : (*bex)["boxes"]) {
            if (b.is_object() && b.value("is_online", 0) == 1)
                ++box_count;
        }
    }
    box_count = std::clamp(box_count, 0, 4);
    if (box_count <= 0)
        return !out_vt_tray.is_null();

    nlohmann::json ams_array  = nlohmann::json::array();
    uint32_t       ams_mask   = 0;
    uint32_t       tray_mask  = 0;

    const nlohmann::json* mcc_box = (mcc && mcc->contains("box") && (*mcc)["box"].is_object()) ? &(*mcc)["box"] : nullptr;
    const nlohmann::json* boxes_arr = (bex && bex->contains("boxes") && (*bex)["boxes"].is_array()) ? &(*bex)["boxes"] : nullptr;

    for (int b = 0; b < box_count; ++b) {
        const nlohmann::json* box_obj = (boxes_arr && static_cast<std::size_t>(b) < boxes_arr->size() && (*boxes_arr)[b].is_object())
                                            ? &(*boxes_arr)[b]
                                            : nullptr;

        const std::string bpfx = "box" + std::to_string(b) + "_";
        double box_temp = 0.0;
        int    box_hum  = 0;
        int    dry_state = 0;
        int    dry_remain = 0;

        if (box_obj) {
            if (box_obj->contains("aht20_temp") && (*box_obj)["aht20_temp"].is_number())
                box_temp = (*box_obj)["aht20_temp"].get<double>();
            if (box_obj->contains("aht20_humidity") && (*box_obj)["aht20_humidity"].is_number())
                box_hum = static_cast<int>(std::lround((*box_obj)["aht20_humidity"].get<double>()));
            dry_state  = box_obj->value("dry_state", 0);
            dry_remain = box_obj->value("end_time", 0);
        } else if (mcc_box) {
            if (mcc_box->contains(bpfx + "aht20_temp") && (*mcc_box)[bpfx + "aht20_temp"].is_number())
                box_temp = (*mcc_box)[bpfx + "aht20_temp"].get<double>();
            if (mcc_box->contains(bpfx + "aht20_humidity") && (*mcc_box)[bpfx + "aht20_humidity"].is_number())
                box_hum = static_cast<int>(std::lround((*mcc_box)[bpfx + "aht20_humidity"].get<double>()));
            dry_state  = mcc_box->value(bpfx + "dry_state", 0);
            dry_remain = mcc_box->value(bpfx + "end_time", 0);
        }

        ams_mask |= (1u << b);
        nlohmann::json unit = nlohmann::json::object();
        unit["id"]           = std::to_string(b);
        {
            std::ostringstream ts;
            ts << std::fixed << std::setprecision(1) << box_temp;
            unit["temp"] = ts.str();
        }
        unit["humidity"]     = humidity_percent_to_level(box_hum);
        unit["humidity_raw"] = std::to_string(std::max(0, box_hum));
        unit["dry_time"]     = (dry_state == 1 && dry_remain > 0) ? dry_remain : 0;

        nlohmann::json tray_arr = nlohmann::json::array();
        const nlohmann::json* fila_arr = (box_obj && box_obj->contains("filament_info") && (*box_obj)["filament_info"].is_array())
                                             ? &(*box_obj)["filament_info"]
                                             : nullptr;
        // Also support nested multi_color_controller.box.box_<b_idx>.filament_info form
        if (!fila_arr && mcc_box) {
            const std::string nested_key = "box_" + std::to_string(b);
            if (mcc_box->contains(nested_key) && (*mcc_box)[nested_key].is_object() &&
                (*mcc_box)[nested_key].contains("filament_info") && (*mcc_box)[nested_key]["filament_info"].is_array()) {
                fila_arr = &(*mcc_box)[nested_key]["filament_info"];
            }
        }

        for (int s = 0; s < 4; ++s) {
            const int global_slot = b * 4 + s;
            std::string qfid;
            std::string ftype;
            std::string rgba = "FFFFFFFF";
            int         min_temp = 0;
            int         max_temp = 0;

            if (fila_arr && static_cast<std::size_t>(s) < fila_arr->size() && (*fila_arr)[s].is_object()) {
                const auto& fi = (*fila_arr)[s];
                qfid     = fi.value("filament_id", std::string{});
                ftype    = fi.value("filament_type", fi.value("type_str", std::string{}));
                rgba     = extract_color_rgba8(fi, "color_rgb_str");
                min_temp = fi.value("min_temp", 0);
                max_temp = fi.value("max_temp", 0);
            } else if (mcc_box) {
                const std::string spfx = bpfx + "filament_info_" + std::to_string(s) + "_";
                qfid     = mcc_box->value(spfx + "filament_id", std::string{});
                ftype    = mcc_box->value(spfx + "filament_type", std::string{});
                rgba     = extract_color_rgba8(*mcc_box, (spfx + "color_rgb_str").c_str());
                min_temp = mcc_box->value(spfx + "min_temp", 0);
                max_temp = mcc_box->value(spfx + "max_temp", 0);
            } else if (sv_vars) {
                const std::string fslot_key = "filament_slot" + std::to_string(global_slot);
                const std::string cslot_key = "color_slot" + std::to_string(global_slot);
                if (sv_vars->contains(fslot_key) && (*sv_vars)[fslot_key].is_string())
                    qfid = (*sv_vars)[fslot_key].get<std::string>();
                rgba = extract_color_rgba8(*sv_vars, cslot_key.c_str());
            }

            nlohmann::json tray_obj = nlohmann::json::object();
            tray_obj["id"] = std::to_string(s);

            if (!qfid.empty() || !ftype.empty()) {
                QidiFilamentEntry entry;
                std::string orca_id = translate_qidi_to_orca_id(qfid);
                const bool  has_entry = lookup_filament_by_orca_id(orca_id, entry) || lookup_filament_by_qidi_id(qfid, entry);
                if (ftype.empty() && has_entry)
                    ftype = entry.type;
                ftype = normalize_filament_type(ftype.empty() ? "PLA" : ftype);
                if (orca_id.empty() || orca_id == qfid)
                    orca_id = fallback_orca_id_for_type(ftype);
                if (min_temp <= 0)
                    min_temp = has_entry ? entry.nozzle_temp_min : 190;
                if (max_temp <= 0)
                    max_temp = has_entry ? entry.nozzle_temp_max : 240;

                tray_mask |= (1u << global_slot);
                tray_obj["tray_info_idx"]   = orca_id;
                tray_obj["tray_type"]       = ftype;
                tray_obj["tray_color"]      = rgba;
                tray_obj["cols"]            = nlohmann::json::array({rgba});
                tray_obj["ctype"]           = 0;
                tray_obj["nozzle_temp_min"] = std::to_string(min_temp);
                tray_obj["nozzle_temp_max"] = std::to_string(max_temp);
            }

            tray_arr.push_back(std::move(tray_obj));
        }

        unit["tray"] = std::move(tray_arr);
        ams_array.push_back(std::move(unit));
    }

    out_ams = nlohmann::json::object();
    out_ams["ams"]             = std::move(ams_array);
    out_ams["ams_exist_bits"]  = format_hex_bits(ams_mask);
    out_ams["tray_exist_bits"] = format_hex_bits(tray_mask);
    out_ams["tray_now"]        = tray_now_str;
    out_ams["version"]         = 1;
    return true;
}

void QidiPrinterAgent::augment_print_payload_locked(const nlohmann::json&        status,
                                                    const std::set<std::string>& objects,
                                                    nlohmann::json&              print_payload) const
{
    print_payload["nozzle_temp_range"]        = {0, 375};
    print_payload["bed_temp_range"]           = {0, 125};
    print_payload["support_ams_humidity"]     = true;
    print_payload["support_timelapse"]        = true;
    print_payload["support_ams_ext_mix_print"] = true;
    print_payload["support_print_without_sd"] = true;
    print_payload["support_send_to_sd"]       = true;
    print_payload["support_polar_cooler"]     = (objects.count("output_pin polar_cooler") != 0);

    if (objects.count("gcode_macro G31") != 0) {
        print_payload["support_bed_leveling"] = 1;
    }

    // Chamber temperature & heater target
    if (objects.count("heater_generic chamber") != 0) {
        print_payload["support_chamber"]                 = true;
        print_payload["support_chamber_temp_edit"]       = true;
        print_payload["support_chamber_temp_edit_range"] = {0, 65};
        if (status.contains("heater_generic chamber") && status["heater_generic chamber"].is_object()) {
            const auto& ch = status["heater_generic chamber"];
            if (ch.contains("temperature") && ch["temperature"].is_number())
                print_payload["chamber_temper"] = ch["temperature"].get<float>();
            if (ch.contains("target") && ch["target"].is_number())
                print_payload["ctt"] = static_cast<int>(std::lround(ch["target"].get<double>()));
        }
    }

    // QIDI 3-fan architecture (cooling_fan P0, auxiliary_cooling_fan P2, chamber_circulation_fan P3)
    const bool has_cooling = objects.count("fan_generic cooling_fan") != 0;
    const bool has_aux     = objects.count("fan_generic auxiliary_cooling_fan") != 0;
    const bool has_chamber = objects.count("fan_generic chamber_circulation_fan") != 0;
    if (has_cooling || has_aux || has_chamber) {
        auto read_fan_pwm = [&](const char* key) -> int {
            if (status.contains(key) && status[key].is_object() &&
                status[key].contains("speed") && status[key]["speed"].is_number()) {
                const double s = std::clamp(status[key]["speed"].get<double>(), 0.0, 1.0);
                return static_cast<int>(std::lround(s * 255.0));
            }
            return 0;
        };
        const int p0 = read_fan_pwm("fan_generic cooling_fan");
        const int p2 = read_fan_pwm("fan_generic auxiliary_cooling_fan");
        const int p3 = read_fan_pwm("fan_generic chamber_circulation_fan");
        print_payload["support_aux_fan"]     = has_aux;
        print_payload["support_chamber_fan"] = has_chamber;
        print_payload["fan_gear"]            = ((p3 & 0xFF) << 16) | ((p2 & 0xFF) << 8) | (p0 & 0xFF);
        print_payload["cooling_fan_speed"]   = std::to_string(static_cast<int>(std::lround(p0 * 100.0 / 255.0)));
        print_payload["big_fan1_speed"]      = std::to_string(static_cast<int>(std::lround(p2 * 100.0 / 255.0)));
        print_payload["big_fan2_speed"]      = std::to_string(static_cast<int>(std::lround(p3 * 100.0 / 255.0)));
    }

    // Caselight LED status
    if (objects.count("output_pin caselight") != 0 &&
        status.contains("output_pin caselight") && status["output_pin caselight"].is_object()) {
        const double val = status["output_pin caselight"].value("value", 0.0);
        print_payload["lights_report"] = nlohmann::json::array({
            nlohmann::json{{"node", "chamber_light"}, {"mode", val > 0.01 ? "on" : "off"}}
        });
    }

    // QIDI Box & External Spool (slot16)
    nlohmann::json ams_json;
    nlohmann::json vt_tray_json;
    if (parse_qidi_ams_status(status, ams_json, vt_tray_json)) {
        if (!ams_json.is_null() && !ams_json.empty())
            print_payload["ams"] = std::move(ams_json);
        if (!vt_tray_json.is_null() && !vt_tray_json.empty())
            print_payload["vt_tray"] = std::move(vt_tray_json);
    }
}

std::string QidiPrinterAgent::sanitize_hex_rgb6(const std::string& raw_color)
{
    std::string hex;
    hex.reserve(8);
    for (unsigned char c : raw_color) {
        if (std::isxdigit(c))
            hex.push_back(static_cast<char>(std::toupper(c)));
    }
    if (hex.size() >= 6)
        return hex.substr(0, 6);
    return "FAFAFA";
}

std::string QidiPrinterAgent::sanitize_single_quoted(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c != '\'' && c != '"' && c != '\n' && c != '\r')
            out.push_back(c);
    }
    return out;
}

std::string QidiPrinterAgent::build_filament_update_script(int                slot_index,
                                                           const std::string& color_hex,
                                                           const std::string& vendor,
                                                           const std::string& filament_str)
{
    const std::string clean_color = sanitize_hex_rgb6(color_hex);
    const std::string clean_vend  = sanitize_single_quoted(vendor.empty() ? "Generic" : vendor);
    const std::string clean_fila  = sanitize_single_quoted(filament_str.empty() ? "PLA" : filament_str);
    return "UPDATE_FILAMENT_INFORMATION SLOT=" + std::to_string(slot_index) +
           " COLOR='" + clean_color + "'" +
           " VENDOR='" + clean_vend + "'" +
           " FILAMENT='" + clean_fila + "'";
}

int QidiPrinterAgent::current_loaded_qidi_slot() const
{
    const auto status = get_status_cache_snapshot();
    if (status.contains("multi_color_controller") && status["multi_color_controller"].is_object() &&
        status["multi_color_controller"].contains("box") && status["multi_color_controller"]["box"].is_object()) {
        const auto& box = status["multi_color_controller"]["box"];
        const int s = parse_slot_string(box.value("loaded_slot", box.value("last_load_slot", std::string{})));
        if (s >= 0)
            return s;
    }
    if (status.contains("save_variables") && status["save_variables"].is_object() &&
        status["save_variables"].contains("variables") && status["save_variables"]["variables"].is_object()) {
        const int s = parse_slot_string(status["save_variables"]["variables"].value("last_load_slot", std::string{}));
        if (s >= 0)
            return s;
    }
    return -1;
}

int QidiPrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
{
    const auto root = nlohmann::json::parse(json_str, nullptr, false);
    if (!root.is_discarded()) {
        // System LED control (Caselight)
        if (root.contains("system") && root["system"].is_object()) {
            const auto& sys = root["system"];
            if (sys.value("command", std::string{}) == "ledctrl" &&
                sys.value("led_node", std::string{}) == "chamber_light") {
                const bool turn_on = (sys.value("led_mode", std::string{}) == "on");
                send_gcode_async(dev_id, turn_on ? "SET_PIN PIN=caselight VALUE=1" : "SET_PIN PIN=caselight VALUE=0");
                return BAMBU_NETWORK_SUCCESS;
            }
        }

        if (root.contains("print") && root["print"].is_object()) {
            const auto& pr  = root["print"];
            const std::string cmd = pr.value("command", std::string{});

            // Chamber temperature target
            if (cmd == "set_ctt") {
                const int temp = std::clamp(pr.value("ctt_val", 0), 0, 70);
                send_gcode_async(dev_id, "M141 S" + std::to_string(temp));
                return BAMBU_NETWORK_SUCCESS;
            }

            // Fan speed slider control (1 = Part P0, 2 = Aux P2, 3 = Chamber P3)
            if (cmd == "set_fan") {
                const int fan_idx = pr.value("fan_index", 1);
                const int pct     = std::clamp(pr.value("speed", 0), 0, 100);
                const int pwm     = static_cast<int>(std::lround(pct * 2.55));
                int       p_idx   = 0;
                if (fan_idx == 2)
                    p_idx = 2;
                else if (fan_idx == 3)
                    p_idx = 3;
                send_gcode_async(dev_id, "M106 P" + std::to_string(p_idx) + " S" + std::to_string(pwm));
                return BAMBU_NETWORK_SUCCESS;
            }

            // Edit QIDI Box slot or External Spool (slot 16) filament info
            if (cmd == "ams_filament_setting") {
                const int ams_id  = pr.value("ams_id", 0);
                const int tray_id = pr.value("tray_id", 0);
                const int slot    = (ams_id >= 254 || tray_id >= 254) ? 16 : (ams_id * 4 + tray_id);
                if (slot >= 0 && slot <= 16) {
                    const std::string info_idx   = pr.value("tray_info_idx", std::string{});
                    const std::string tray_color = pr.value("tray_color", std::string{"FAFAFAFF"});
                    const std::string tray_type  = pr.value("tray_type", std::string{"PLA"});

                    QidiFilamentEntry entry;
                    std::string fw_vendor   = "Generic";
                    std::string fw_filament = tray_type.empty() ? "PLA" : tray_type;
                    if (lookup_filament_by_orca_id(info_idx, entry) || lookup_filament_by_qidi_id(info_idx, entry)) {
                        fw_vendor   = entry.firmware_vendor;
                        fw_filament = entry.firmware_filament;
                    }
                    send_gcode_async(dev_id, build_filament_update_script(slot, tray_color, fw_vendor, fw_filament));
                    return BAMBU_NETWORK_SUCCESS;
                }
            }

            // Load / Unload filament on QIDI Box or External Spool
            if (cmd == "ams_change_filament") {
                const int target = pr.value("target", -1);
                if (target == 255) {
                    // Unload currently active slot
                    const int cur_slot = current_loaded_qidi_slot();
                    if (cur_slot >= 0 && cur_slot < 16) {
                        send_gcode_async(dev_id, "UNLOAD_FILAMENT_CONTROL SLOT=" + std::to_string(cur_slot));
                    } else {
                        send_gcode_async(dev_id, "M603");
                    }
                    return BAMBU_NETWORK_SUCCESS;
                }
                if (target == 254) {
                    send_gcode_async(dev_id, "M604");
                    return BAMBU_NETWORK_SUCCESS;
                }
                if (target >= 0 && target < 16) {
                    send_gcode_async(dev_id, "LOAD_FILAMENT_CONTROL SLOT=" + std::to_string(target));
                    return BAMBU_NETWORK_SUCCESS;
                }
            }

            // Stop active QIDI Box drying
            if (cmd == "auto_stop_ams_dry") {
                send_gcode_async(dev_id, "STOP_BOX_DRYING BOX=0");
                return BAMBU_NETWORK_SUCCESS;
            }
        }
    }

    return MoonrakerPrinterAgent::handle_request(dev_id, json_str);
}

bool QidiPrinterAgent::fetch_filament_info(std::string /*dev_id*/, FilamentSyncMode /*sync_mode*/)
{
    std::string error;

    std::string series_id;
    {
        MoonrakerDeviceInfo info;
        if (fetch_device_info(device_info.base_url, device_info.api_key, info, error, {})) {
            series_id = infer_series_id(info.model_id, info.dev_name);
        }
    }
    if (series_id.empty()) {
        series_id = infer_series_id(device_info.model_id, device_info.model_name);
    }

    QidiFilamentDict dict;
    if (!fetch_filament_dict(device_info.base_url, device_info.api_key, dict, error)) {
        BOOST_LOG_TRIVIAL(debug) << "QidiPrinterAgent::fetch_filament_info: Filament dict optional fetch skipped: " << error;
    }

    std::vector<AmsTrayData> trays;
    int                      box_count = 0;
    nlohmann::json           raw_status;
    bool                     slots_ok = false;
    try {
        slots_ok = fetch_slot_info(device_info.base_url, device_info.api_key, dict, series_id, trays, box_count, raw_status, error);
    } catch (const nlohmann::json::exception& e) {
        error = e.what();
    }
    if (!slots_ok) {
        BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
        return false;
    }

    build_ams_payload(box_count, box_count * 4 - 1, trays);

    // Also populate rich AMS metadata (humidity, temperature, dry_time, tray_now) and External Spool (vt_tray)
    if (wxTheApp != nullptr) {
        if (DeviceManager* dev = GUI::wxGetApp().getDeviceManager()) {
            if (MachineObject* obj = dev->get_selected_machine()) {
                nlohmann::json ams_json;
                nlohmann::json vt_tray_json;
                if (parse_qidi_ams_status(raw_status, ams_json, vt_tray_json)) {
                    nlohmann::json wrapper = nlohmann::json::object();
                    if (!ams_json.is_null() && !ams_json.empty())
                        wrapper["print"]["ams"] = std::move(ams_json);
                    if (!vt_tray_json.is_null() && !vt_tray_json.empty())
                        wrapper["print"]["vt_tray"] = std::move(vt_tray_json);
                    if (wrapper.contains("print"))
                        obj->parse_json("lan", wrapper.dump());
                }
            }
        }
    }

    return true;
}

std::string QidiPrinterAgent::build_tool_slot_script(const std::string& ams_mapping)
{
    // The firmware has value_t0 to value_t15 and slots 0 to 15
    constexpr int slot_count = 16;

    // One entry per filament: its Box slot index, negative when no slot was chosen
    const auto  mapping = nlohmann::json::parse(ams_mapping, nullptr, false);
    std::string script;
    if (!mapping.is_array()) {
        return script;
    }
    for (int filament = 0; filament < int(mapping.size()) && filament < slot_count; ++filament) {
        if (!mapping[filament].is_number_integer()) {
            continue;
        }
        const int slot = mapping[filament].get<int>();
        if (slot < 0 || slot >= slot_count) {
            continue;
        }
        if (!script.empty()) {
            script += '\n';
        }
        // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name keeps its own quotes
        script += "SAVE_VARIABLE VARIABLE=value_t" + std::to_string(filament) + " VALUE=\"'slot" + std::to_string(slot) + "'\"";
    }
    return script;
}

std::string QidiPrinterAgent::build_pre_print_script(const PrintParams&           params,
                                                     const std::set<std::string>& available_objects)
{
    std::vector<std::string> lines;

    const bool has_box_hw = (available_objects.empty() ||
                             available_objects.count("multi_color_controller") != 0 ||
                             available_objects.count("box_extras") != 0);
    const bool use_box    = params.task_use_ams && has_mapped_box_slot(params);

    if (has_box_hw) {
        lines.push_back(std::string("SAVE_VARIABLE VARIABLE=enable_box VALUE=") + (use_box ? "1" : "0"));
        if (use_box) {
            const std::string tool_script = build_tool_slot_script(params.ams_mapping);
            if (!tool_script.empty())
                lines.push_back(tool_script);
        }
    } else if (use_box) {
        const std::string tool_script = build_tool_slot_script(params.ams_mapping);
        if (!tool_script.empty())
            lines.push_back(tool_script);
    }

    if (available_objects.count("gcode_macro G31") != 0 && available_objects.count("gcode_macro G32") != 0) {
        lines.push_back(params.task_bed_leveling ? "G31" : "G32");
    }

    if (available_objects.count("output_pin polar_cooler") != 0) {
        bool enable_polar = false;
        if (!params.extra_options.empty()) {
            const auto extra = nlohmann::json::parse(params.extra_options, nullptr, false);
            if (extra.is_object()) {
                enable_polar = extra.value("enable_polar_cooler", false);
            }
        }
        lines.push_back(std::string("SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=") + (enable_polar ? "1" : "0"));
    }

    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            joined += '\n';
        joined += lines[i];
    }
    return joined;
}

bool QidiPrinterAgent::set_timelapse_enabled(bool enabled) const
{
    if (device_info.base_url.empty())
        return false;
    const std::string url = join_url(device_info.base_url,
                                     std::string("/machine/timelapse/settings?enabled=") + (enabled ? "true" : "false"));
    bool ok = false;
    auto http = Http::post(url);
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .timeout_connect(4)
        .timeout_max(8)
        .on_complete([&](std::string /*body*/, unsigned status) { ok = (status >= 200 && status < 300); })
        .on_error([&](std::string /*body*/, std::string err, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << "QidiPrinterAgent: timelapse setting skipped (" << status << "): " << err;
        })
        .perform_sync();
    return ok;
}

int QidiPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return MoonrakerPrinterAgent::start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

int QidiPrinterAgent::start_local_print_with_record(PrintParams      params,
                                                    OnUpdateStatusFn update_fn,
                                                    WasCancelledFn   cancel_fn,
                                                    OnWaitFn         /*wait_fn*/)
{
    return start_local_print(std::move(params), std::move(update_fn), std::move(cancel_fn));
}

bool QidiPrinterAgent::print_uploaded_file(const PrintParams& params, const std::string& filename)
{
    const auto        objects = get_available_objects_snapshot();
    const std::string script  = build_pre_print_script(params, objects);
    if (!script.empty() && !send_gcode_sync(device_info.dev_id, script)) {
        return false;
    }

    // Best-effort Moonraker timelapse toggle (matches QIDIStudio SelectMachine.cpp:3405)
    set_timelapse_enabled(params.task_record_timelapse);

    // On the X-Max 4 and Q2, Klipper only opens the copy Moonraker's print start makes, so SDCARD_PRINT_FILE can't find the upload
    std::string response;
    return send_jsonrpc_command(device_info.base_url, device_info.api_key, print_start_request(params, filename), response);
}

nlohmann::json QidiPrinterAgent::print_start_request(const PrintParams& params, const std::string& filename)
{
    nlohmann::json request{{"filename", filename}};
    // Qidi's Moonraker prints Metadata/plate_<plateindex>.gcode from a 3MF, defaulting to plate 1
    if (boost::iends_with(filename, ".3mf")) {
        const int plate = params.plate_index > 0 ? params.plate_index : 1;
        request["plateindex"] = std::to_string(plate);
    }
    return request;
}

bool QidiPrinterAgent::fetch_slot_info(const std::string&        base_url,
                                       const std::string&        api_key,
                                       const QidiFilamentDict&   dict,
                                       const std::string&        series_id,
                                       std::vector<AmsTrayData>& trays,
                                       int&                      box_count,
                                       nlohmann::json&           raw_status,
                                       std::string&              error)
{
    std::string url = join_url(base_url, "/printer/objects/query?save_variables=variables&multi_color_controller&box_extras");
    for (int i = 0; i < 16; ++i) {
        url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    }

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string /*body*/, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded() || !json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].is_object()) {
        error = "Invalid JSON response";
        return false;
    }

    raw_status = json["result"]["status"];

    // Modern QIDI Box path (X-Max 4 / Q2 / Plus 4 with multi_color_controller or box_extras)
    nlohmann::json ams_json;
    nlohmann::json vt_tray_json;
    if (parse_qidi_ams_status(raw_status, ams_json, vt_tray_json) &&
        ams_json.is_object() && ams_json.contains("ams") && ams_json["ams"].is_array()) {
        box_count = static_cast<int>(ams_json["ams"].size());
        trays.clear();
        trays.reserve(box_count * 4);
        for (int b = 0; b < box_count; ++b) {
            const auto& unit = ams_json["ams"][b];
            if (!unit.contains("tray") || !unit["tray"].is_array())
                continue;
            for (const auto& t : unit["tray"]) {
                const int local_s = std::stoi(t.value("id", std::string{"0"}));
                AmsTrayData tray;
                tray.slot_index    = b * 4 + local_s;
                tray.tray_info_idx = t.value("tray_info_idx", std::string{});
                tray.tray_type     = t.value("tray_type", std::string{});
                tray.tray_color    = t.value("tray_color", std::string{"FFFFFFFF"});
                tray.has_filament  = !tray.tray_info_idx.empty() || !tray.tray_type.empty();
                trays.push_back(std::move(tray));
            }
        }
        return true;
    }

    // Legacy fallback (older save_variables dictionary-indexed Qidi Box firmware)
    if (!raw_status.contains("save_variables") || !raw_status["save_variables"].contains("variables")) {
        error = "Unexpected JSON structure";
        return false;
    }

    auto& variables = raw_status["save_variables"]["variables"];
    box_count       = std::max(0, variables.value("box_count", 1));
    const int max_slots = box_count * 4;
    trays.clear();
    trays.reserve(max_slots);

    auto build_setting_id = [&](int filament_type_idx, int vendor_type, const std::string& tray_type) {
        const int vendor = (vendor_type == 1) ? 1 : 0;
        if (is_numeric(series_id) && filament_type_idx > 0) {
            return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(filament_type_idx);
        }
        return map_filament_type_to_setting_id(tray_type);
    };

    for (int i = 0; i < max_slots; ++i) {
        AmsTrayData tray;
        tray.slot_index = i;

        const auto color_value    = variables.value("color_slot" + std::to_string(i), nlohmann::json(1));
        const auto filament_value = variables.value("filament_slot" + std::to_string(i), nlohmann::json(1));
        const int  color_index    = color_value.is_number() ? color_value.get<int>() : 0;
        const int  filament_type  = filament_value.is_number() ? filament_value.get<int>() : 0;
        const int  vendor_type    = variables.value("vendor_slot" + std::to_string(i), 0);

        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        tray.has_filament = false;
        if (raw_status.contains(box_stepper_key)) {
            auto& box_stepper = raw_status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                int runout_button = box_stepper["runout_button"].template get<int>();
                tray.has_filament = (runout_button == 0);
            }
        }

        if (tray.has_filament) {
            std::string filament_name = "PLA";
            auto filament_it = dict.filaments.find(filament_type);
            if (filament_it != dict.filaments.end()) {
                filament_name = filament_it->second;
            }
            tray.tray_type = normalize_filament_type(filament_name);

            std::string setting_id = build_setting_id(filament_type, vendor_type, tray.tray_type);
            auto* bundle = (wxTheApp != nullptr) ? GUI::wxGetApp().preset_bundle : nullptr;
            if (!bundle) {
                tray.tray_info_idx = setting_id;
            } else if (!setting_id.empty() && has_visible_base_preset(bundle->filaments, setting_id)) {
                tray.tray_info_idx = setting_id;
            } else {
                tray.tray_info_idx = fallback_orca_id_for_type(tray.tray_type);
            }

            auto color_it = dict.colors.find(color_index);
            if (color_value.is_array() && !color_value.empty() && color_value[0].is_string()) {
                tray.tray_color = to_rgba8_hex(color_value[0].get<std::string>());
            } else if (color_it != dict.colors.end()) {
                tray.tray_color = to_rgba8_hex(color_it->second);
            } else {
                tray.tray_color = "FFFFFFFF";
            }
        }

        trays.push_back(tray);
    }

    return true;
}

bool QidiPrinterAgent::fetch_filament_dict(const std::string& base_url,
                                           const std::string& api_key,
                                           QidiFilamentDict&  dict,
                                           std::string&       error) const
{
    std::string url = join_url(base_url, "/server/files/config/officiall_filas_list.cfg");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!api_key.empty()) {
        http.header("X-Api-Key", api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string /*body*/, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    dict.colors.clear();
    dict.filaments.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);

    return !dict.colors.empty();
}

void QidiPrinterAgent::parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    bool               in_section     = false;
    std::string        section_header = "[" + section_name + "]";

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            in_section = (line == section_header);
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (in_section) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    int index     = std::stoi(key);
                    result[index] = value;
                } catch (...) {}
            }
        }
    }
}

void QidiPrinterAgent::parse_filament_sections(const std::string& content, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    int                current_fila_index = -1;

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            current_fila_index = -1;
            if (line.size() > 5 && line.substr(0, 5) == "[fila" && line.back() == ']') {
                std::string num_str = line.substr(5, line.size() - 6);
                try {
                    current_fila_index = std::stoi(num_str);
                } catch (...) {
                    current_fila_index = -1;
                }
            }
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (current_fila_index > 0) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                if (key == "filament") {
                    result[current_fila_index] = value;
                }
            }
        }
    }
}

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    if (upper == "PLA") {
        return "QD_1_0_1";
    }
    if (upper == "ABS") {
        return "QD_1_0_11";
    }
    if (upper == "PETG") {
        return "QD_1_0_41";
    }
    if (upper == "TPU") {
        return "QD_1_0_50";
    }
    return "";
}

std::string QidiPrinterAgent::normalize_model_key(std::string value)
{
    boost::algorithm::to_lower(value);
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            normalized.push_back(static_cast<char>(c));
        }
    }
    return normalized;
}

std::string QidiPrinterAgent::infer_series_id(const std::string& model_id, const std::string& dev_name)
{
    std::string source = model_id.empty() ? dev_name : model_id;
    boost::trim(source);
    if (source.empty()) {
        return "";
    }
    if (is_numeric(source)) {
        return source;
    }

    const std::string key = normalize_model_key(source);
    if (key.find("q2") != std::string::npos) {
        return "1";
    }
    if (key.find("xmax") != std::string::npos && key.find("4") != std::string::npos) {
        return "3";
    }
    if ((key.find("xplus") != std::string::npos || key.find("plus") != std::string::npos) && key.find("4") != std::string::npos) {
        return "0";
    }
    return "";
}

std::string QidiPrinterAgent::normalize_filament_type(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    if (upper.find("PLA") != std::string::npos)
        return "PLA";
    if (upper.find("ABS") != std::string::npos)
        return "ABS";
    if (upper.find("PETG") != std::string::npos)
        return "PETG";
    if (upper.find("TPU") != std::string::npos)
        return "TPU";
    if (upper.find("ASA") != std::string::npos)
        return "ASA";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";
    if (upper.find("PC") != std::string::npos)
        return "PC";
    if (upper.find("PVA") != std::string::npos)
        return "PVA";

    return upper;
}

} // namespace Slic3r
