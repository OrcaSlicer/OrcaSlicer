#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

SCENARIO("Printer camera configuration storage", "[Config][Camera]") {
    GIVEN("An AppConfig instance") {
        AppConfig config;

        WHEN("No printer camera is configured") {
            THEN("has_printer_camera returns false") {
                REQUIRE_FALSE(config.has_printer_camera("dev123"));
            }
            THEN("get_printer_camera returns empty config") {
                auto cam = config.get_printer_camera("dev123");
                REQUIRE(cam.dev_id.empty());
                REQUIRE(cam.custom_source.empty());
                REQUIRE_FALSE(cam.enabled);
            }
            THEN("get_all_printer_cameras returns empty map") {
                REQUIRE(config.get_all_printer_cameras().empty());
            }
        }

        WHEN("A printer camera is configured") {
            PrinterCameraConfig cam_config;
            cam_config.dev_id = "ABC123";
            cam_config.custom_source = "http://camera.local/stream";
            cam_config.enabled = true;
            config.set_printer_camera(cam_config);

            THEN("has_printer_camera returns true") {
                REQUIRE(config.has_printer_camera("ABC123"));
            }
            THEN("get_printer_camera returns correct values") {
                auto cam = config.get_printer_camera("ABC123");
                REQUIRE(cam.dev_id == "ABC123");
                REQUIRE(cam.custom_source == "http://camera.local/stream");
                REQUIRE(cam.enabled == true);
            }
            THEN("Other printers are unaffected") {
                REQUIRE_FALSE(config.has_printer_camera("XYZ789"));
            }
            THEN("get_all_printer_cameras contains the config") {
                auto all = config.get_all_printer_cameras();
                REQUIRE(all.size() == 1);
                REQUIRE(all.count("ABC123") == 1);
            }
        }

        WHEN("A printer camera is configured with enabled=false") {
            PrinterCameraConfig cam_config;
            cam_config.dev_id = "DEV456";
            cam_config.custom_source = "rtsp://camera.local:554/stream";
            cam_config.enabled = false;
            config.set_printer_camera(cam_config);

            THEN("has_printer_camera returns true") {
                REQUIRE(config.has_printer_camera("DEV456"));
            }
            THEN("get_printer_camera returns enabled=false") {
                auto cam = config.get_printer_camera("DEV456");
                REQUIRE(cam.enabled == false);
            }
        }

        WHEN("Multiple printer cameras are configured") {
            PrinterCameraConfig cam1;
            cam1.dev_id = "dev1";
            cam1.custom_source = "http://cam1";
            cam1.enabled = true;

            PrinterCameraConfig cam2;
            cam2.dev_id = "dev2";
            cam2.custom_source = "http://cam2";
            cam2.enabled = false;

            PrinterCameraConfig cam3;
            cam3.dev_id = "dev3";
            cam3.custom_source = "rtsp://cam3:554/live";
            cam3.enabled = true;

            config.set_printer_camera(cam1);
            config.set_printer_camera(cam2);
            config.set_printer_camera(cam3);

            THEN("get_all_printer_cameras returns all configs") {
                auto all = config.get_all_printer_cameras();
                REQUIRE(all.size() == 3);
                REQUIRE(all.count("dev1") == 1);
                REQUIRE(all.count("dev2") == 1);
                REQUIRE(all.count("dev3") == 1);
            }
            THEN("Each config can be retrieved independently") {
                REQUIRE(config.get_printer_camera("dev1").custom_source == "http://cam1");
                REQUIRE(config.get_printer_camera("dev2").custom_source == "http://cam2");
                REQUIRE(config.get_printer_camera("dev3").custom_source == "rtsp://cam3:554/live");
            }
            THEN("Each config preserves its enabled state") {
                REQUIRE(config.get_printer_camera("dev1").enabled == true);
                REQUIRE(config.get_printer_camera("dev2").enabled == false);
                REQUIRE(config.get_printer_camera("dev3").enabled == true);
            }
        }

        WHEN("A printer camera is erased") {
            PrinterCameraConfig cam;
            cam.dev_id = "dev_to_delete";
            cam.custom_source = "http://will_be_deleted";
            cam.enabled = true;
            config.set_printer_camera(cam);

            REQUIRE(config.has_printer_camera("dev_to_delete"));
            config.erase_printer_camera("dev_to_delete");

            THEN("has_printer_camera returns false") {
                REQUIRE_FALSE(config.has_printer_camera("dev_to_delete"));
            }
            THEN("get_printer_camera returns empty config") {
                auto result = config.get_printer_camera("dev_to_delete");
                REQUIRE(result.dev_id.empty());
            }
        }

        WHEN("Erasing a non-existent printer camera") {
            config.erase_printer_camera("nonexistent_dev");

            THEN("No exception is thrown and state remains consistent") {
                REQUIRE_FALSE(config.has_printer_camera("nonexistent_dev"));
                REQUIRE(config.get_all_printer_cameras().empty());
            }
        }

        WHEN("A printer camera config is updated") {
            PrinterCameraConfig cam;
            cam.dev_id = "dev_update";
            cam.custom_source = "http://old_url";
            cam.enabled = false;
            config.set_printer_camera(cam);

            cam.custom_source = "http://new_url";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("New values are stored") {
                auto result = config.get_printer_camera("dev_update");
                REQUIRE(result.custom_source == "http://new_url");
                REQUIRE(result.enabled == true);
            }
            THEN("Only one entry exists in the map") {
                REQUIRE(config.get_all_printer_cameras().size() == 1);
            }
        }

        WHEN("One of multiple cameras is erased") {
            PrinterCameraConfig cam1;
            cam1.dev_id = "keep1";
            cam1.custom_source = "http://keep1";
            cam1.enabled = true;

            PrinterCameraConfig cam2;
            cam2.dev_id = "delete_me";
            cam2.custom_source = "http://delete";
            cam2.enabled = true;

            PrinterCameraConfig cam3;
            cam3.dev_id = "keep2";
            cam3.custom_source = "http://keep2";
            cam3.enabled = false;

            config.set_printer_camera(cam1);
            config.set_printer_camera(cam2);
            config.set_printer_camera(cam3);

            config.erase_printer_camera("delete_me");

            THEN("Only the specified camera is removed") {
                REQUIRE(config.has_printer_camera("keep1"));
                REQUIRE_FALSE(config.has_printer_camera("delete_me"));
                REQUIRE(config.has_printer_camera("keep2"));
            }
            THEN("Remaining cameras retain their values") {
                REQUIRE(config.get_printer_camera("keep1").custom_source == "http://keep1");
                REQUIRE(config.get_printer_camera("keep2").custom_source == "http://keep2");
            }
            THEN("Map size decreases by one") {
                REQUIRE(config.get_all_printer_cameras().size() == 2);
            }
        }
    }
}

SCENARIO("PrinterCameraConfig struct equality", "[Config][Camera]") {
    GIVEN("Two PrinterCameraConfig instances") {
        PrinterCameraConfig config1;
        config1.dev_id = "test_dev";
        config1.custom_source = "http://test";
        config1.enabled = true;

        PrinterCameraConfig config2;
        config2.dev_id = "test_dev";
        config2.custom_source = "http://test";
        config2.enabled = true;

        WHEN("All fields are identical") {
            THEN("They are equal") {
                REQUIRE(config1 == config2);
                REQUIRE_FALSE(config1 != config2);
            }
        }

        WHEN("dev_id differs") {
            config2.dev_id = "different_dev";
            THEN("They are not equal") {
                REQUIRE_FALSE(config1 == config2);
                REQUIRE(config1 != config2);
            }
        }

        WHEN("custom_source differs") {
            config2.custom_source = "http://different";
            THEN("They are not equal") {
                REQUIRE_FALSE(config1 == config2);
                REQUIRE(config1 != config2);
            }
        }

        WHEN("enabled differs") {
            config2.enabled = false;
            THEN("They are not equal") {
                REQUIRE_FALSE(config1 == config2);
                REQUIRE(config1 != config2);
            }
        }
    }
}

SCENARIO("PrinterCameraConfig handles various URL formats", "[Config][Camera]") {
    GIVEN("An AppConfig instance") {
        AppConfig config;

        WHEN("HTTP URL is stored") {
            PrinterCameraConfig cam;
            cam.dev_id = "http_test";
            cam.custom_source = "http://192.168.1.100:8080/stream";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("URL is preserved exactly") {
                REQUIRE(config.get_printer_camera("http_test").custom_source == "http://192.168.1.100:8080/stream");
            }
        }

        WHEN("HTTPS URL is stored") {
            PrinterCameraConfig cam;
            cam.dev_id = "https_test";
            cam.custom_source = "https://secure.camera.com/live";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("URL is preserved exactly") {
                REQUIRE(config.get_printer_camera("https_test").custom_source == "https://secure.camera.com/live");
            }
        }

        WHEN("RTSP URL is stored") {
            PrinterCameraConfig cam;
            cam.dev_id = "rtsp_test";
            cam.custom_source = "rtsp://user:pass@camera.local:554/stream1";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("URL is preserved exactly including credentials") {
                REQUIRE(config.get_printer_camera("rtsp_test").custom_source == "rtsp://user:pass@camera.local:554/stream1");
            }
        }

        WHEN("URL with special characters is stored") {
            PrinterCameraConfig cam;
            cam.dev_id = "special_test";
            cam.custom_source = "http://camera.local/stream?quality=high&fps=30";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("URL is preserved with query parameters") {
                REQUIRE(config.get_printer_camera("special_test").custom_source == "http://camera.local/stream?quality=high&fps=30");
            }
        }

        WHEN("Empty URL is stored") {
            PrinterCameraConfig cam;
            cam.dev_id = "empty_url_test";
            cam.custom_source = "";
            cam.enabled = false;
            config.set_printer_camera(cam);

            THEN("Empty URL is preserved") {
                auto result = config.get_printer_camera("empty_url_test");
                REQUIRE(result.custom_source.empty());
                REQUIRE(result.enabled == false);
            }
        }
    }
}

SCENARIO("PrinterCameraConfig handles various dev_id formats", "[Config][Camera]") {
    GIVEN("An AppConfig instance") {
        AppConfig config;

        WHEN("Standard serial number is used") {
            PrinterCameraConfig cam;
            cam.dev_id = "01P00A123456789";
            cam.custom_source = "http://test";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("Serial number is stored correctly") {
                REQUIRE(config.has_printer_camera("01P00A123456789"));
            }
        }

        WHEN("UUID-style dev_id is used") {
            PrinterCameraConfig cam;
            cam.dev_id = "550e8400-e29b-41d4-a716-446655440000";
            cam.custom_source = "http://test";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("UUID is stored correctly") {
                REQUIRE(config.has_printer_camera("550e8400-e29b-41d4-a716-446655440000"));
            }
        }

        WHEN("Short dev_id is used") {
            PrinterCameraConfig cam;
            cam.dev_id = "ABC";
            cam.custom_source = "http://test";
            cam.enabled = true;
            config.set_printer_camera(cam);

            THEN("Short ID is stored correctly") {
                REQUIRE(config.has_printer_camera("ABC"));
            }
        }
    }
}
