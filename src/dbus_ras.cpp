#include "Config.hpp"
#include "cper.hpp"
#include "ras.hpp"

static std::shared_ptr<sdbusplus::asio::connection> conn;
static std::shared_ptr<sdbusplus::asio::object_server> server;
static std::array<
    std::pair<std::string, std::shared_ptr<sdbusplus::asio::dbus_interface>>,
    MAX_ERROR_FILE>
    crashdumpInterfaces;

constexpr std::string_view crashdumpService = "com.amd.crashdump";
constexpr std::string_view crashdumpPath = "/com/amd/crashdump";
constexpr std::string_view crashdumpInterface = "com.amd.crashdump";
constexpr std::string_view dimmEccPath = "/com/amd/crashdump/dramEccPath";

constexpr std::string_view deleteAllInterface =
    "xyz.openbmc_project.Collection.DeleteAll";
constexpr std::string_view deleteAllMethod = "DeleteAll";
constexpr std::string_view crashdumpAssertedInterface =
    "com.amd.crashdump.Asserted";
constexpr std::string_view crashdumpConfigInterface =
    "com.amd.crashdump.Configuration";
constexpr std::string_view dimmEccInterface =
    "com.amd.crashdump.DimmEcc.ErrorCount";

constexpr std::string_view crashdumpAssertedMethod = "GenerateAssertedLog";
// These 2 interfaces will be called by bmcweb, not supported now.
// constexpr std::string_view crashdumpOnDemandInterface =
// "com.amd.crashdump.OnDemand"; constexpr std::string_view
// crashdumpOnDemandMethod = "GenerateOnDemandLog"; constexpr std::string_view
// crashdumpTelemetryInterface = "com.amd.crashdump.Telemetry"; constexpr
// std::string_view crashdumpTelemetryMethod = "GenerateTelemetryLog";

constexpr int kCrashdumpTimeInSec = 300;

std::vector<std::pair<std::string, uint64_t>> P0_DimmEccCount = {
    {"P0_DIMM_A", 0}, {"P0_DIMM_A1", 0}, {"P0_DIMM_B", 0}, {"P0_DIMM_B1", 0},
    {"P0_DIMM_C", 0}, {"P0_DIMM_C1", 0}, {"P0_DIMM_D", 0}, {"P0_DIMM_D1", 0},
    {"P0_DIMM_E", 0}, {"P0_DIMM_E1", 0}, {"P0_DIMM_F", 0}, {"P0_DIMM_F1", 0},
    {"P0_DIMM_G", 0}, {"P0_DIMM_G1", 0}, {"P0_DIMM_H", 0}, {"P0_DIMM_H1", 0},
    {"P0_DIMM_I", 0}, {"P0_DIMM_I1", 0}, {"P0_DIMM_J", 0}, {"P0_DIMM_J1", 0},
    {"P0_DIMM_K", 0}, {"P0_DIMM_K1", 0}, {"P0_DIMM_L", 0}, {"P0_DIMM_L1", 0}};

std::vector<std::pair<std::string, uint64_t>> P1_DimmEccCount = {
    {"P1_DIMM_A", 0}, {"P1_DIMM_A1", 0}, {"P1_DIMM_B", 0}, {"P1_DIMM_B1", 0},
    {"P1_DIMM_C", 0}, {"P1_DIMM_C1", 0}, {"P1_DIMM_D", 0}, {"P1_DIMM_D1", 0},
    {"P1_DIMM_E", 0}, {"P1_DIMM_E1", 0}, {"P1_DIMM_F", 0}, {"P1_DIMM_F1", 0},
    {"P1_DIMM_G", 0}, {"P1_DIMM_G1", 0}, {"P1_DIMM_H", 0}, {"P1_DIMM_H1", 0},
    {"P1_DIMM_I", 0}, {"P1_DIMM_I1", 0}, {"P1_DIMM_J", 0}, {"P1_DIMM_J1", 0},
    {"P1_DIMM_K", 0}, {"P1_DIMM_K1", 0}, {"P1_DIMM_L", 0}, {"P1_DIMM_L1", 0}};

template <typename T>
void updateConfigFile(std::string jsonField, T updateData)
{
    std::ifstream jsonRead(config_file);
    nlohmann::json data = nlohmann::json::parse(jsonRead);

    if constexpr (std::is_same_v<
                      T, std::vector<std::pair<std::string, std::string>>>)
    {
        nlohmann::json obj;
        for (const auto& pair : updateData)
        {
            obj[pair.first] = pair.second;
        }
        data[jsonField] = obj;
    }
    else
    {
        data[jsonField] = updateData;
    }

    std::ofstream jsonWrite(config_file);
    jsonWrite << data;

    jsonRead.close();
    jsonWrite.close();
}

std::string findCperFilename(int number)
{
    std::regex pattern(".*" + std::to_string(number) + "\\.cper");

    for (const auto& entry :
         std::filesystem::directory_iterator(kRasDir.data()))
    {
        std::string filename = entry.path().filename().string();
        if (std::regex_match(filename, pattern))
        {
            return filename;
        }
    }

    return "";
}

void exportCrashdumpToDBus(int num, const ERROR_TIME_STAMP& TimeStampStr)
{

    if (num < 0 || num >= MAX_ERROR_FILE)
    {
        sd_journal_print(LOG_ERR, "Crashdump only allows index 0~9\n");
        return;
    }

    // remove the interface if it exists
    if (crashdumpInterfaces[num].second != nullptr)
    {
        server->remove_interface(crashdumpInterfaces[num].second);
        crashdumpInterfaces[num].second.reset();
    }

    const std::string filename = findCperFilename(num);
    const std::string fullFilePath = kRasDir.data() + filename;

    // Use ISO-8601 as the timestamp format
    // For example: 2022-07-19T14:13:47Z
    const ERROR_TIME_STAMP& t = TimeStampStr;
    char timestamp[30];
    sprintf(timestamp, "%d-%d-%dT%d:%d:%dZ", (t.Century - 1) * 100 + t.Year,
            t.Month, t.Day, t.Hours, t.Minutes, t.Seconds);

    // Create crashdump DBus instance
    const std::string dbusPath =
        std::string{crashdumpPath} + "/" + std::to_string(num);
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
        server->add_interface(dbusPath, crashdumpInterface.data());
    iface->register_property("Log", fullFilePath);
    iface->register_property("Filename", filename);
    iface->register_property("Timestamp", std::string{timestamp});
    iface->initialize();

    crashdumpInterfaces[num] = {filename, iface};
}

void CreateDbusInterface()
{
    std::future<void> fut;

    conn = std::make_shared<sdbusplus::asio::connection>(io);
    conn->request_name(crashdumpService.data());
    server = std::make_shared<sdbusplus::asio::object_server>(conn);

    // This DBus interface/method should be triggered by
    // host-error-monitor(https://github.com/openbmc/host-error-monitor).
    // However `amd-ras` monitors the alert pin by itself instead of asking
    // `host-error-monitor` to do so. So currently no service will call this
    // DBus method (this still can be called by `busctl` CLI).

    // Generate crashdump and expose on DBus when APML_ALERT pin is asserted
    std::shared_ptr<sdbusplus::asio::dbus_interface> assertedIface =
        server->add_interface(crashdumpPath.data(),
                              crashdumpAssertedInterface.data());
    assertedIface->register_method(
        crashdumpAssertedMethod.data(), [&fut](const std::string& alertName) {
            // Do nothing if logging is in progress already
            if (fut.valid() && fut.wait_for(std::chrono::seconds(0)) !=
                                   std::future_status::ready)
            {
                return "Logging is in progress already";
            }

            fut = std::async(std::launch::async, [&alertName]() {});
            return "Log started";
        });
    assertedIface->initialize();

    std::shared_ptr<sdbusplus::asio::dbus_interface> dimmEccIface =
        server->add_interface(dimmEccPath.data(), dimmEccInterface.data());

    dimmEccIface
        ->register_property_r<std::vector<std::pair<std::string, uint64_t>>>(
            "P0_DIMM_ECC_COUNT", P0_DimmEccCount,
            sdbusplus::vtable::property_::emits_change,
            [](const auto&) { return P0_DimmEccCount; });

    dimmEccIface
        ->register_property_r<std::vector<std::pair<std::string, uint64_t>>>(
            "P1_DIMM_ECC_COUNT", P1_DimmEccCount,
            sdbusplus::vtable::property_::emits_change,
            [](const auto&) { return P1_DimmEccCount; });

    dimmEccIface->initialize();

    // Create Configuration interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> configIface =
        server->add_interface(crashdumpPath.data(),
                              crashdumpConfigInterface.data());

    uint16_t apmlRetryCount = Configuration::getApmlRetryCount();
    configIface->register_property(
        "apmlRetries", apmlRetryCount,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setApmlRetryCount(resp);
            updateConfigFile("apmlRetries", resp);
            return true;
        });

    uint16_t systemRecovery = Configuration::getSystemRecovery();
    configIface->register_property(
        "systemRecovery", systemRecovery,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setSystemRecovery(resp);
            updateConfigFile("systemRecovery", resp);
            return true;
        });

    bool harvestuCodeVersionFlag = Configuration::getHarvestuCodeVersionFlag();
    configIface->register_property(
        "harvestuCodeVersion", harvestuCodeVersionFlag,
        [](const bool& requested, bool& resp)

        {
            resp = requested;
            Configuration::setHarvestuCodeVersionFlag(resp);
            updateConfigFile("harvestuCodeVersion", resp);
            return true;
        });

    bool harvestPpinFlag = Configuration::getHarvestPpinFlag();
    configIface->register_property("harvestPpin", harvestPpinFlag,
                                   [](const bool& requested, bool& resp) {
                                       resp = requested;
                                       Configuration::setHarvestPpinFlag(resp);
                                       updateConfigFile("harvestPpin", resp);
                                       return true;
                                   });

    std::string ResetSignal = Configuration::getResetSignal();
    configIface->register_property(
        "ResetSignal", ResetSignal,
        [](const std::string& requested, std::string& resp) {
            resp = requested;
            Configuration::setResetSignal(resp);
            updateConfigFile("ResetSignal", resp);
            return true;
        });

    std::vector<std::string> sigIDOffset = Configuration::getSigIDOffset();
    configIface->register_property("sigIDOffset", sigIDOffset,
                                   [](const std::vector<std::string>& requested,
                                      std::vector<std::string>& resp) {
                                       resp = requested;
                                       Configuration::setSigIDOffset(resp);
                                       updateConfigFile("sigIDOffset", resp);
                                       return true;
                                   });

    std::vector<std::pair<std::string, std::string>> P0_DimmLabels =
        Configuration::getAllP0_DimmLabels();
    configIface->register_property(
        "P0_DIMM_LABELS", P0_DimmLabels,
        [](const std::vector<std::pair<std::string, std::string>>& requested,
           std::vector<std::pair<std::string, std::string>>& resp) {
            for (const auto& keyValuePair : requested)
            {
                const std::string& key = keyValuePair.first;
                const std::string& value = keyValuePair.second;

                for (auto& pair : resp)
                {
                    if (pair.first == key)
                    {
                        pair.second = value;
                        break;
                    }
                }
            }

            Configuration::setAllP0_DimmLabels(resp);
            updateConfigFile("P0_DIMM_LABELS", resp);
            return true;
        });

    std::vector<std::pair<std::string, std::string>> P1_DimmLabels =
        Configuration::getAllP1_DimmLabels();
    configIface->register_property(
        "P1_DIMM_LABELS", P1_DimmLabels,
        [](const std::vector<std::pair<std::string, std::string>>& requested,
           std::vector<std::pair<std::string, std::string>>& resp) {
            for (const auto& keyValuePair : requested)
            {
                const std::string& key = keyValuePair.first;
                const std::string& value = keyValuePair.second;

                for (auto& pair : resp)
                {
                    if (pair.first == key)
                    {
                        pair.second = value;
                        break;
                    }
                }
            }

            Configuration::setAllP1_DimmLabels(resp);
            updateConfigFile("P1_DIMM_LABELS", resp);
            return true;
        });

    bool AifsArmed = Configuration::getAifsArmed();
    configIface->register_property("AifsArmed", AifsArmed,
                                   [](const bool& requested, bool& resp) {
                                       resp = requested;
                                       Configuration::setAifsArmed(resp);
                                       updateConfigFile("AifsArmed", resp);
                                       return true;
                                   });

    std::vector<std::pair<std::string, std::string>> AifsSignatureId =
        Configuration::getAllAifsSignatureId();
    configIface->register_property(
        "AifsSignatureId", AifsSignatureId,
        [](const std::vector<std::pair<std::string, std::string>>& requested,
           std::vector<std::pair<std::string, std::string>>& resp) {
            resp = requested;
            Configuration::setAllAifsSignatureId(resp);
            updateConfigFile("AifsSignatureId", resp);
            return true;
        });

    bool McaPollingEn = Configuration::getMcaPollingEn();
    configIface->register_property("McaPollingEn", McaPollingEn,
                                   [](const bool& requested, bool& resp) {
                                       resp = requested;
                                       Configuration::setMcaPollingEn(resp);
                                       updateConfigFile("McaPollingEn", resp);
                                       return true;
                                   });

    bool DramCeccPollingEn = Configuration::getDramCeccPollingEn();
    configIface->register_property(
        "DramCeccPollingEn", DramCeccPollingEn,
        [](const bool& requested, bool& resp) {
            resp = requested;
            Configuration::setDramCeccPollingEn(resp);
            updateConfigFile("DramCeccPollingEn", resp);
            return true;
        });

    bool PcieAerPollingEn = Configuration::getPcieAerPollingEn();
    configIface->register_property("PcieAerPollingEn", PcieAerPollingEn,
                                   [](const bool& requested, bool& resp) {
                                       resp = requested;
                                       Configuration::setPcieAerPollingEn(resp);
                                       updateConfigFile("PcieAerPollingEn",
                                                        resp);
                                       return true;
                                   });

    bool McaThresholdEn = Configuration::getMcaThresholdEn();
    configIface->register_property("McaThresholdEn", McaThresholdEn,
                                   [](const bool& requested, bool& resp) {
                                       resp = requested;
                                       Configuration::setMcaThresholdEn(resp);
                                       updateConfigFile("McaThresholdEn", resp);
                                       return true;
                                   });

    bool DramCeccThresholdEn = Configuration::getDramCeccThresholdEn();
    configIface->register_property(
        "DramCeccThresholdEn", DramCeccThresholdEn,
        [](const bool& requested, bool& resp) {
            resp = requested;
            Configuration::setDramCeccThresholdEn(resp);
            updateConfigFile("DramCeccThresholdEn", resp);
            return true;
        });

    bool PcieAerThresholdEn = Configuration::getPcieAerThresholdEn();
    configIface->register_property(
        "PcieAerThresholdEn", PcieAerThresholdEn,
        [](const bool& requested, bool& resp) {
            resp = requested;
            Configuration::setPcieAerThresholdEn(resp);
            updateConfigFile("PcieAerThresholdEn", resp);
            return true;
        });

    uint16_t McaPollingPeriod = Configuration::getMcaPollingPeriod();
    configIface->register_property(
        "McaPollingPeriod", McaPollingPeriod,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setMcaPollingPeriod(resp);
            updateConfigFile("McaPollingPeriod", resp);
            return true;
        });

    uint16_t DramCeccPollingPeriod = Configuration::getDramCeccPollingPeriod();
    configIface->register_property(
        "DramCeccPollingPeriod", DramCeccPollingPeriod,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setDramCeccPollingPeriod(resp);
            updateConfigFile("DramCeccPollingPeriod", resp);
            return true;
        });

    uint16_t PcieAerPollingPeriod = Configuration::getPcieAerPollingPeriod();
    configIface->register_property(
        "PcieAerPollingPeriod", PcieAerPollingPeriod,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setPcieAerPollingPeriod(resp);
            updateConfigFile("PcieAerPollingPeriod", resp);
            return true;
        });

    uint16_t McaErrThresholdCnt = Configuration::getMcaErrThresholdCnt();
    configIface->register_property(
        "McaErrThresholdCnt", McaErrThresholdCnt,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setMcaErrThresholdCnt(resp);
            updateConfigFile("McaErrThresholdCnt", resp);
            return true;
        });

    uint16_t DramCeccErrThresholdCnt = Configuration::getDramCeccErrThresholdCnt();
    configIface->register_property(
        "DramCeccErrThresholdCnt", DramCeccErrThresholdCnt,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setDramCeccErrThresholdCnt(resp);
            updateConfigFile("DramCeccErrThresholdCnt", resp);
            return true;
        });

    uint16_t PcieAerErrThresholdCnt = Configuration::getPcieAerErrThresholdCnt();
    configIface->register_property(
        "PcieAerErrThresholdCnt", PcieAerErrThresholdCnt,
        [](const uint16_t& requested, uint16_t& resp) {
            resp = requested;
            Configuration::setPcieAerErrThresholdCnt(resp);
            updateConfigFile("PcieAerErrThresholdCnt", resp);
            return true;
        });

    configIface->initialize();

    // Delete all the generated crashdump
    std::shared_ptr<sdbusplus::asio::dbus_interface> deleteAllIface =
        server->add_interface(crashdumpPath.data(), deleteAllInterface.data());
    deleteAllIface->register_method(deleteAllMethod.data(), [&fut]() {
        if (fut.valid() &&
            fut.wait_for(std::chrono::seconds(kCrashdumpTimeInSec)) !=
                std::future_status::ready)
        {
            sd_journal_print(
                LOG_WARNING,
                "A logging is still in progress, that one won't get removed\n");
        }
        for (auto& [filename, interface] : crashdumpInterfaces)
        {
            if (!std::filesystem::remove(
                    std::filesystem::path(kRasDir.data() + filename)))
            {
                sd_journal_print(LOG_WARNING, "Can't remove crashdump %s\n",
                                 filename.c_str());
            }
            server->remove_interface(interface);
            filename = "";
            interface = nullptr;
        }
        return "Logs cleared";
    });
    deleteAllIface->initialize();

    // com.amd.crashdump.OnDemand/GenerateOnDemandLog currently not supported
    // com.amd.crashdump.Telemetry/GenerateTelemetryLog currently not supported

    // Check if any crashdump already exists.
    if (std::filesystem::exists(std::filesystem::path(kRasDir.data())))
    {
        std::regex pattern(".*ras-error([[:digit:]]+).cper");
        std::smatch match;
        for (const auto& p : std::filesystem::directory_iterator(
                 std::filesystem::path(kRasDir.data())))
        {
            std::string filename = p.path().filename();
            if (!std::regex_match(filename, match, pattern))
            {
                continue;
            }
            const int kNum = stoi(match.str(1));
            const std::string cperFilename = kRasDir.data() + filename;
            // exportCrashdumpToDBus needs the timestamp inside the CPER
            // file. So load it first.
            std::ifstream fin(cperFilename, std::ifstream::binary);
            if (!fin.is_open())
            {
                sd_journal_print(LOG_WARNING,
                                 "Broken crashdump CPER file: %s\n",
                                 cperFilename.c_str());
                continue;
            }
            fin.seekg(24); // Move the file pointer to offset 24
            ERROR_TIME_STAMP timestamp;

            if (!fin.read(reinterpret_cast<char*>(&timestamp),
                          sizeof(timestamp)))
            {
                std::cout << "Failed to read data from the file." << std::endl;
            }

            fin.close();
            exportCrashdumpToDBus(kNum, timestamp);
        }
    }
}
