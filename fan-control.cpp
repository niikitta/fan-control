#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fans_control
{

boost::asio::io_service io;

// fans groups to write
static const std::string sysfsFansWritePath =
    "/sys/devices/platform/ahb/ahb:apb/1e786000.pwm-tacho-controller/hwmon/hwmon0/pwm";

// sensors groups to read
static const std::string sysfsSensorsReadPath0 =
    "/xyz/openbmc_project/sensors/temperature/";
static const std::string sysfsSensorsReadPath1 = "";

static void sysfsFansWrite(const int& value)
{
    std::vector<std::ofstream> fans;
    int numFans;
    char fanNumber = '1';

    // open 6 fans files
    for (numFans = 0; numFans < 6; ++numFans, ++fanNumber)
    {
        fans[numFans].open(sysfsFansWritePath + fanNumber);
        if (!fanNumber)
        {
            std::clog << "Failed to open " << numFans << std::endl;
        }
    }

    /*
     *   temps(0 - 90) ---> fans(0 - 255)
     *   [18 - 36]                102
     *   (36 - 54]                153
     *   (54 - 72]                204
     *   (72 - inf                255
     */
    if (value >= 18 && value <= 36)
        for (numFans = 0; numFans < 6; ++numFans)
            fans[numFans] << 102;
    else if (value > 36 && value <= 54)
        for (numFans = 0; numFans < 6; ++numFans)
            fans[numFans] << 153;
    else if (value > 54 && value <= 72)
        for (numFans = 0; numFans < 6; ++numFans)
            fans[numFans] << 204;
    else if (value > 72)
        for (numFans = 0; numFans < 6; ++numFans)
            fans[numFans] << 255;

    for (numFans = 0; numFans < 6; ++numFans, ++fanNumber)
    {
        fans[numFans].close();
    }
}

static void processingValues(const std::vector<int>& values1,
                             const std::vector<int>& values2)
{
    int sum1, sum2, res;
    sum1 = sum2 = 0;

    for (const int& value : values1)
    {
        sum1 += value;
    }
    for (const int& value : values2)
    {
        sum1 += value;
    }

    // sum / 5 cores and / 2 procs
    res = (sum1 / 5 + sum2 / 5) / 2;
    sysfsFansWrite(res);
}

static void readSensorValues()
{
    std::vector<std::ifstream> cpu0;
    std::vector<std::ifstream> cpu1;
    std::vector<int> cpuValues0;
    std::vector<int> cpuValues1;
    int numOfCores = 5;
    char chCore = '1';
    char temperature;

    // cpu0 open
    for (int core0; core0 < numOfCores; ++core0, ++chCore)
    {
        cpu0[core0].open(sysfsSensorsReadPath0 + "Core_" + chCore + "_CPU0");
        cpu0[core0] >> temperature;
        cpuValues0.push_back(static_cast<int>(temperature) / 1000);
        cpu0[core0].close();
    }
    // cpu1 open
    for (int core1; core1 < numOfCores; ++core1, ++chCore)
    {
        cpu1[core1].open(sysfsSensorsReadPath1 + "Core_" + chCore + "_CPU1");
        cpu1[core1] >> temperature;
        cpuValues1.push_back(static_cast<int>(temperature) / 1000);
        cpu1[core1].close();
    }
}

static void waitSysfsSensors(const boost::system::error_code&,
                             boost::asio::steady_timer* timer)
{
    readSensorValues();
    timer->expires_at(timer->expiry() + boost::asio::chrono::seconds(1));
    timer->async_wait(
        boost::bind(waitSysfsSensors, boost::asio::placeholders::error, timer));
}

static void prepareWaitSysfs()
{
    boost::asio::steady_timer timer(io, boost::asio::chrono::seconds(1));
    timer.async_wait(boost::bind(waitSysfsSensors,
                                 boost::asio::placeholders::error, &timer));
}

static void run()
{
    prepareWaitSysfs();
}

static void waitPECIBus(const boost::system::error_code&,
                        boost::asio::steady_timer* timer,
                        std::ifstream* checkFile, int* value)
{
    *checkFile >> *value;
    if (value > 0)
    {
        return;
    }
    timer->expires_at(timer->expiry() + boost::asio::chrono::seconds(2));
    timer->async_wait(boost::bind(waitPECIBus, boost::asio::placeholders::error,
                                  timer, checkFile, value));
}

// in short, getting value by sysfs takes more time and this functions check it
static bool checkPeciBus(const std::string pathCheck)
{
    boost::asio::steady_timer timer(io, boost::asio::chrono::seconds(2));
    int value = 0;

    std::ifstream coreTempFile(pathCheck);
    if (!coreTempFile)
    {
        std::cerr << "Failed to open file. Configure PECI bus\n";
        return -1;
    }

    timer.async_wait(boost::bind(waitPECIBus, boost::asio::placeholders::error,
                                 &timer, &coreTempFile, &value));

    if (value > 0)
        return true;
}

}; // namespace fans_control

int main()
{
    using namespace fans_control;

    if (checkPeciBus(sysfsSensorsReadPath0 + "Core_5_CPU0"))
        std::cout << "Start fans control...\n";

    run();

    io.run();
    return 0;
}