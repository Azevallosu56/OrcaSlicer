#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/QidiPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;

TEST_CASE("A filament mapped to a Box slot saves that slot in its tool variable", "[QidiPrinterAgent]")
{
    // Filament 2 is mapped to slot 5; filaments 0 and 1 have no slot.
    // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name needs its own quotes.
    CHECK(QidiPrinterAgent::build_tool_slot_script("[-1, -1, 5]") == "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot5'\"");
}

TEST_CASE("Each mapped filament gets its own tool variable line", "[QidiPrinterAgent]")
{
    CHECK(QidiPrinterAgent::build_tool_slot_script("[3, -1, 0]") ==
          "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot3'\"\n"
          "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot0'\"");
}

TEST_CASE("A mapping without a usable Box slot saves nothing", "[QidiPrinterAgent]")
{
    const std::string mapping = GENERATE(as<std::string>{},
                                         "",           // empty mapping
                                         "[]",         // no filaments
                                         "[-1, -1]",   // no filament has a slot
                                         "[16]",       // past the firmware's slot15
                                         "[\"slot1\"]", // not a slot index
                                         "{\"0\": 1}", // not an array
                                         "[1,");       // not valid JSON
    CAPTURE(mapping);
    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping).empty());
}

TEST_CASE("Filaments past the firmware's last tool variable are skipped", "[QidiPrinterAgent]")
{
    // The firmware has value_t0 to value_t15: filament 15 maps to slot 1, filament 16 to slot 4.
    std::string mapping = "[";
    for (int filament = 0; filament < 15; ++filament)
        mapping += "-1, ";
    mapping += "1, 4]";

    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping) == "SAVE_VARIABLE VARIABLE=value_t15 VALUE=\"'slot1'\"");
}

TEST_CASE("The print start of a 3MF names the plate to print", "[QidiPrinterAgent]")
{
    PrintParams params;
    params.plate_index = 2;

    CHECK(QidiPrinterAgent::print_start_request(params, "Cube.gcode.3mf") ==
          nlohmann::json{{"filename", "Cube.gcode.3mf"}, {"plateindex", "2"}});
    CHECK(QidiPrinterAgent::print_start_request(params, "Cube.gcode") == nlohmann::json{{"filename", "Cube.gcode"}});
}

TEST_CASE("Qidi filament catalog translates QF codes to Orca OF IDs and firmware strings", "[QidiPrinterAgent]")
{
    CHECK(QidiPrinterAgent::translate_qidi_to_orca_id("QFA90") == "OFDSrzZ8");
    CHECK(QidiPrinterAgent::translate_qidi_to_orca_id("OFDSrzZ8") == "OFDSrzZ8");
    CHECK(QidiPrinterAgent::translate_orca_to_qidi_id("OF4nQ62r") == "QFA00");

    QidiFilamentEntry entry;
    REQUIRE(QidiPrinterAgent::lookup_filament_by_qidi_id("QFB53", entry));
    CHECK(entry.orca_id == "OFma5TXH");
    CHECK(entry.firmware_vendor == "QIDI");
    CHECK(entry.firmware_filament == "ABS-Odorless");
}

TEST_CASE("Pre-print script configures Qidi Box, G31/G32 leveling, and Polar Cooler", "[QidiPrinterAgent]")
{
    const std::set<std::string> objects{
        "multi_color_controller",
        "gcode_macro G31",
        "gcode_macro G32",
        "output_pin polar_cooler",
    };

    PrintParams box_params;
    box_params.task_use_ams       = true;
    box_params.ams_mapping        = "[0, 3]";
    box_params.task_bed_leveling  = true;
    box_params.extra_options      = R"({"enable_polar_cooler":true})";

    CHECK(QidiPrinterAgent::build_pre_print_script(box_params, objects) ==
          "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n"
          "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot0'\"\n"
          "SAVE_VARIABLE VARIABLE=value_t1 VALUE=\"'slot3'\"\n"
          "G31\n"
          "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=1");

    PrintParams ext_params;
    ext_params.task_use_ams      = false;
    ext_params.ams_mapping       = "[-1]";
    ext_params.task_bed_leveling = false;
    ext_params.extra_options     = R"({"enable_polar_cooler":false})";

    CHECK(QidiPrinterAgent::build_pre_print_script(ext_params, objects) ==
          "SAVE_VARIABLE VARIABLE=enable_box VALUE=0\n"
          "G32\n"
          "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=0");
}

TEST_CASE("Qidi Max 4 Moonraker status populates drying Box slots and slot16 External Spool", "[QidiPrinterAgent]")
{
    const auto status = nlohmann::json::parse(R"({
        "multi_color_controller": {
            "box_count": 1,
            "box": {
                "loaded_slot": "slot16",
                "last_load_slot": "slot16"
            },
            "extra": {
                "identity": "external",
                "filament_info": {
                    "filament_id": "QFA00",
                    "filament_type": "PLA",
                    "color_rgb_str": ["#FAFAFA"],
                    "min_temp": 190,
                    "max_temp": 240
                }
            }
        },
        "box_extras": {
            "boxes": [
                {
                    "is_online": 1,
                    "aht20_humidity": 15,
                    "aht20_temp": 65.17,
                    "dry_state": 1,
                    "end_time": 479,
                    "filament_info": [
                        {"index": 0, "filament_id": "QFA90", "filament_type": "PLA", "color_rgb_str": ["#B87F2B"], "min_temp": 190, "max_temp": 240},
                        {"index": 1, "filament_id": "QFA90", "filament_type": "PLA", "color_rgb_str": ["#F28636"], "min_temp": 190, "max_temp": 240},
                        {"index": 2, "filament_id": "QFA90", "filament_type": "PLA", "color_rgb_str": ["#161616"], "min_temp": 190, "max_temp": 240},
                        {"index": 3, "filament_id": "QFA00", "filament_type": "PLA", "color_rgb_str": ["#F24F8E"], "min_temp": 190, "max_temp": 240}
                    ]
                }
            ]
        },
        "box_stepper slot0": {"runout_button": 1}
    })");

    nlohmann::json ams;
    nlohmann::json vt_tray;
    REQUIRE(QidiPrinterAgent::parse_qidi_ams_status(status, ams, vt_tray));

    CHECK(ams["tray_now"] == "254");
    CHECK(ams["ams_exist_bits"] == "1");
    CHECK(ams["tray_exist_bits"] == "f");
    REQUIRE(ams["ams"].size() == 1);
    CHECK(ams["ams"][0]["temp"] == "65.2");
    CHECK(ams["ams"][0]["humidity_raw"] == "15");
    CHECK(ams["ams"][0]["dry_time"] == 479);
    REQUIRE(ams["ams"][0]["tray"].size() == 4);
    CHECK(ams["ams"][0]["tray"][0]["tray_info_idx"] == "OFDSrzZ8");
    CHECK(ams["ams"][0]["tray"][0]["tray_color"] == "B87F2BFF");
    CHECK(ams["ams"][0]["tray"][3]["tray_info_idx"] == "OF4nQ62r");

    CHECK(vt_tray["id"] == "254");
    CHECK(vt_tray["tray_info_idx"] == "OF4nQ62r");
    CHECK(vt_tray["tray_color"] == "FAFAFAFF");
}

TEST_CASE("Filament update script strips hash and alpha for Klipper UPDATE_FILAMENT_INFORMATION", "[QidiPrinterAgent]")
{
    CHECK(QidiPrinterAgent::build_filament_update_script(16, "#B87F2BFF", "QIDI", "PLA Basic") ==
          "UPDATE_FILAMENT_INFORMATION SLOT=16 COLOR='B87F2B' VENDOR='QIDI' FILAMENT='PLA Basic'");
}
