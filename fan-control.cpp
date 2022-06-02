#include <gpiod.h>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define NUMBER_OF_FANS 4
#define NUMBER_OF_CPUS 2
#define NUMBER_OF_READING_CORES 5

// temperature ranges
#define LOW_TEMPERATURE 50
#define BIG_TEMPERATURE 75
#define VERY_BIG_TEMPERATURE 90

// fan ranges
#define VERY_LOW_FAN 130
#define LOW_BIG_FAN 170
#define BIG_VERY_BIG_FAN 225
#define CRITICAL_FAN 255

#define GPIO_PS_PWROK_OFFSET 30

namespace fans_control
{

boost::asio::io_service io;

// fans groups to write
static std::string sysfsFansWritePath =
    "/sys/devices/platform/ahb/ahb:apb/1e786000.pwm-tacho-controller/hwmon";

// PECI sensors groups to read (2 sockets)
static std::string sysfsSensorsReadPath0 =
    "/sys/devices/platform/ahb/ahb:apb/ahb:apb:bus@1e78b000/1e78b000.peci-bus/peci-0/0-30/peci-cputemp.0/hwmon";
static const std::string sysfsSensorsReadPath1 =
    "/sys/devices/platform/ahb/ahb:apb/ahb:apb:bus@1e78b000/1e78b000.peci-bus/peci-0/0-31/peci-cputemp.0/hwmon";

static bool readyToRead = false;

static std::vector<std::string> idsWithCoreTemps;

typedef struct
{
    const char* device = "gpiochip0";
    unsigned int offset = GPIO_PS_PWROK_OFFSET;
    bool aclive_low = false;
    const char* consumer = "fan-control";
    int flags = 0;
} gpioPsPowerOkConfig;
gpioPsPowerOkConfig psPowerOkConfig;

static void findIdsWithCoreTemps(std::string& findPath)
{
    // start from 6 id
    int probeId = 6;
    std::string findStr;
    while (probeId)
    {
        std::ifstream probeFile(sysfsSensorsReadPath0 + "temp" +
                                std::to_string(probeId) + "_label");
        if (probeFile.is_open())
        {
            probeFile >> findStr;
            if (findStr.find("Core") != std::string::npos)
            {
                idsWithCoreTemps.push_back(std::to_string(probeId));
            }
        }
        if (idsWithCoreTemps.size() == NUMBER_OF_READING_CORES)
            return;
        probeFile.close();
        ++probeId;
    }
}

// through well-known path function get id bad known path
static void makePathForHwmon(std::string& modifyPath)
{
    char id;
    for (const auto& dir : std::filesystem::directory_iterator(modifyPath))
        id = *(--dir.path().string().end());
    modifyPath = modifyPath + "/hwmon" + id + '/';
}

static void sysfsFansWrite(const int& fanRate, std::string& fansWritePath)
{
    std::ofstream fanFile;
    static int currentFanRate;
    char chFan = '1';

    if (!(currentFanRate == fanRate))
    {
        currentFanRate = fanRate;
        std::cout << "Write fan rate: " << fanRate << "\n";
        for (int fan = 0; fan < NUMBER_OF_FANS; ++fan, ++chFan)
        {
            fanFile.open(fansWritePath + "pwm" + chFan);
            if (!fanFile.is_open())
            {
                std::cerr << "Failed to open fan file: " << fansWritePath
                          << "pwm" << chFan << '\n';
                return;
            }
            fanFile << fanRate;
            fanFile.close();
        }
    }
}

static void processingValues(const std::vector<int>& tempsValues)
{
    int fanRate;
    int temperature = 0;

    for (const auto& value : tempsValues)
    {
        if (value > temperature)
            temperature = value;
    }
    if (temperature <= LOW_TEMPERATURE)
    {
        fanRate = VERY_LOW_FAN;
    }
    else if (temperature > LOW_TEMPERATURE && temperature <= BIG_TEMPERATURE)
    {
        fanRate = LOW_BIG_FAN;
    }
    else if (temperature > BIG_TEMPERATURE &&
             temperature <= VERY_BIG_TEMPERATURE)
    {
        fanRate = BIG_VERY_BIG_FAN;
    }
    else if (temperature > VERY_BIG_TEMPERATURE)
    {
        fanRate = CRITICAL_FAN;
    }
    sysfsFansWrite(fanRate, sysfsFansWritePath);
}

static void readSensorValues(std::string& sensorsPath)
{
    std::ifstream sensorFile;

    // 0...4 - cpu1, 5...9 - cpu2
    std::vector<int> cpusTemperature(10, 0);
    std::string temperature;
    auto idsIter = idsWithCoreTemps.cbegin();

    for (int core = 0; core < NUMBER_OF_READING_CORES * 2; ++core, ++idsIter)
    {
        if (core < NUMBER_OF_READING_CORES)
        {
            sensorFile.open(sensorsPath + "temp" + *idsIter + "_input");
            if (!sensorFile.is_open())
            {
                std::cerr << "Failed to open sensor file: " << sensorsPath
                          << "temp" << *idsIter << "_input\n";
                return;
            }
            sensorFile >> temperature;
            cpusTemperature.push_back(atoi(temperature.c_str()));
            sensorFile.close();
        }
#ifdef CPU2_ENABLE
        else
        {
            if (core == NUMBER_OF_READING_CORES)
                idsIter = idsWithCoreTemps.cbegin();
            sensorFile.open(sysfsSensorsReadPath1 + "temp" + *idsIter +
                            "_input");
            if (!sensorFile.is_open())
            {
                std::cerr << "Failed to open sensor file: " << sensorsPath
                          << "temp" << *idsIter << "_input\n";
                return;
            }
            sensorFile >> temperature;
            cpusTemperature.push_back(atoi(temperature.c_str()));
            sensorFile.close();
        }
#endif
    }
    // make degrees celsius
    for (auto& value : cpusTemperature)
    {
        value /= 1000;
    }

    processingValues(cpusTemperature);
}

static void waitSysfsSensors(const boost::system::error_code&,
                             boost::asio::deadline_timer* timer)
{
    if (readyToRead)
    {
        std::cout << "Start reading...\n";
        readSensorValues(sysfsSensorsReadPath0);
    }
    timer->expires_at(timer->expires_at() + boost::posix_time::seconds(1));
    timer->async_wait(
        boost::bind(waitSysfsSensors, boost::asio::placeholders::error, timer));
}

/*  In result, PECI bus need some seconds to initialize and show temperature
 *  values in sysfs.
 */
static void waitPECIBus(const boost::system::error_code&,
                        boost::asio::deadline_timer* timer,
                        std::string* tempValue, int* boardState)
{
    std::ifstream peciTemp(sysfsSensorsReadPath0 + "temp" +
                           *idsWithCoreTemps.cbegin() + "_input");

    // get server state
    *boardState = gpiod_ctxless_get_value_ext(
        psPowerOkConfig.device, psPowerOkConfig.offset,
        psPowerOkConfig.aclive_low, psPowerOkConfig.consumer,
        psPowerOkConfig.flags);

    if (*boardState)
    {
        if (!peciTemp)
        {
            std::cerr
                << "check PECI temp: Failed to read peci temp file. Check PECI configuration...\n";
        }
        peciTemp >> *tempValue;
        std::cout << "tempValue: " << *tempValue << '\n';
        if (!tempValue->empty())
        {
            std::cout << "check PECI temp: PECI ready to read\n";
            readyToRead = true;
        }
        else if (tempValue->empty())
        {
            std::cout << "check PECI temp: wait PECI...\n";
        }
    }
    else
    {
        std::cout << "skip: power off\n";
        *tempValue = "";
        if (readyToRead)
            readyToRead = false;
    }

    timer->expires_at(timer->expires_at() + boost::posix_time::seconds(1));
    timer->async_wait(boost::bind(waitPECIBus, boost::asio::placeholders::error,
                                  timer, tempValue, boardState));
}

}; // namespace fans_control

int main()
{
    using namespace fans_control;

    makePathForHwmon(sysfsFansWritePath);
    makePathForHwmon(sysfsSensorsReadPath0);

    findIdsWithCoreTemps(sysfsSensorsReadPath0);

    boost::asio::deadline_timer waitPECITimer(io,
                                              boost::posix_time::seconds(1));
    std::string tempValue = "";
    int boardState;
    waitPECITimer.async_wait(
        boost::bind(waitPECIBus, boost::asio::placeholders::error,
                    &waitPECITimer, &tempValue, &boardState));

    boost::asio::deadline_timer fansControllerTimer(
        io, boost::posix_time::seconds(1));
    fansControllerTimer.async_wait(boost::bind(waitSysfsSensors,
                                               boost::asio::placeholders::error,
                                               &fansControllerTimer));
    io.run();
    return 0;
}