/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/hardware/automotive/can/1.0/ICanBus.h>
#include <android/hardware/automotive/can/1.0/ICanController.h>
#include <android/hardware/automotive/can/1.0/types.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <can-vts-utils/bus-enumerator.h>
#include <can-vts-utils/can-hal-printers.h>
#include <gmock/gmock.h>
#include <hidl-utils/hidl-utils.h>
#include <hidl/GtestPrinter.h>
#include <hidl/ServiceManagement.h>

namespace android::hardware::automotive::can::V1_0::vts {

using hardware::hidl_vec;
using InterfaceType = ICanController::InterfaceType;
using IfId = ICanController::BusConfig::InterfaceId;

struct Bus {
    DISALLOW_COPY_AND_ASSIGN(Bus);

    Bus(sp<ICanController> controller, const ICanController::BusConfig& config)
        : mIfname(config.name), mController(controller) {
        // Don't bring up the bus, we just need a wrapper for the ICanBus object
        /* Not using ICanBus::getService here, since it ignores interfaces not in the manifest
         * file -- this is a test, so we don't want to add fake services to a device manifest. */
        auto manager = hidl::manager::V1_2::IServiceManager::getService();
        auto service = manager->get(ICanBus::descriptor, config.name);
        mBus = ICanBus::castFrom(service);
    }

    ICanBus* operator->() const { return mBus.get(); }
    sp<ICanBus> get() { return mBus; }

    Return<Result> send(const CanMessage& msg) { return mBus->send(msg); }

  private:
    const std::string mIfname;
    sp<ICanController> mController;
    sp<ICanBus> mBus;
};

class CanControllerHalTest : public ::testing::TestWithParam<std::string> {
  protected:
    virtual void SetUp() override;
    virtual void TearDown() override;
    static void SetUpTestCase();

    Bus makeBus(const std::string ifaceName);

    hidl_vec<InterfaceType> getSupportedInterfaceTypes();
    bool isSupported(InterfaceType iftype);

    bool up(InterfaceType iftype, const std::string srvname, std::string ifname,
            ICanController::Result expected);
    void assertRegistered(const std::string srvname, const std::string ifaceName,
                          bool expectRegistered);

    sp<ICanController> mCanController;
    static hidl_vec<hidl_string> mBusNames;

  private:
    static bool mTestCaseInitialized;
};

hidl_vec<hidl_string> CanControllerHalTest::mBusNames;
bool CanControllerHalTest::mTestCaseInitialized = false;

void CanControllerHalTest::SetUp() {
    ASSERT_TRUE(mTestCaseInitialized);

    mCanController = ICanController::getService(GetParam());
    ASSERT_TRUE(mCanController) << "Couldn't open CAN Controller: " << GetParam();
}

void CanControllerHalTest::TearDown() {
    mCanController.clear();
}

void CanControllerHalTest::SetUpTestCase() {
    mBusNames = utils::getBusNames();
    ASSERT_NE(0u, mBusNames.size()) << "No ICanBus HALs defined in device manifest";

    mTestCaseInitialized = true;
}

hidl_vec<InterfaceType> CanControllerHalTest::getSupportedInterfaceTypes() {
    hidl_vec<InterfaceType> iftypesResult;
    mCanController->getSupportedInterfaceTypes(hidl_utils::fill(&iftypesResult)).assertOk();
    return iftypesResult;
}

bool CanControllerHalTest::isSupported(InterfaceType iftype) {
    const auto supported = getSupportedInterfaceTypes();
    return std::find(supported.begin(), supported.end(), iftype) != supported.end();
}

bool CanControllerHalTest::up(InterfaceType iftype, std::string srvname, std::string ifname,
                              ICanController::Result expected) {
    ICanController::BusConfig config = {};
    config.name = srvname;

    // TODO(b/146214370): move interfaceId constructors to a library
    if (iftype == InterfaceType::SOCKETCAN) {
        IfId::Socketcan socketcan = {};
        socketcan.ifname(ifname);
        config.interfaceId.socketcan(socketcan);
    } else if (iftype == InterfaceType::SLCAN) {
        IfId::Slcan slcan = {};
        slcan.ttyname(ifname);
        config.interfaceId.slcan(slcan);
    } else if (iftype == InterfaceType::VIRTUAL) {
        config.interfaceId.virtualif({ifname});
    } else {
        EXPECT_TRUE(false) << "Unexpected iftype: " << toString(iftype);
    }

    const auto upresult = mCanController->upInterface(config);

    if (!isSupported(iftype)) {
        LOG(INFO) << iftype << " interfaces not supported";
        EXPECT_EQ(ICanController::Result::NOT_SUPPORTED, upresult);
        return false;
    }

    EXPECT_EQ(expected, upresult);
    return true;
}

void CanControllerHalTest::assertRegistered(const std::string srvname, const std::string ifaceName,
                                            bool expectRegistered) {
    /* Not using ICanBus::tryGetService here, since it ignores interfaces not in the manifest
     * file -- this is a test, so we don't want to add fake services to a device manifest. */
    auto manager = hidl::manager::V1_2::IServiceManager::getService();
    auto busService = manager->get(ICanBus::descriptor, srvname);
    if (!expectRegistered) {
        /* We can't unregister a HIDL interface defined in the manifest, so we'll just check to make
         * sure that the interface behind it is down */
        auto bus = makeBus(ifaceName);
        const auto result = bus->send({});
        ASSERT_EQ(Result::INTERFACE_DOWN, result);
        return;
    }
    ASSERT_EQ(expectRegistered, busService.withDefault(nullptr) != nullptr)
            << "ICanBus/" << srvname << (expectRegistered ? " is not " : " is ") << "registered"
            << " (should be otherwise)";
}

Bus CanControllerHalTest::makeBus(const std::string ifaceName) {
    ICanController::BusConfig config = {};
    config.name = mBusNames[0];
    config.interfaceId.virtualif({ifaceName});

    return Bus(mCanController, config);
}

TEST_P(CanControllerHalTest, SupportsSomething) {
    const auto supported = getSupportedInterfaceTypes();
    ASSERT_GT(supported.size(), 0u);
}

TEST_P(CanControllerHalTest, BringUpDown) {
    const std::string name = mBusNames[0];
    const std::string iface = "vcan57";
    mCanController->downInterface(name);

    assertRegistered(name, iface, false);

    if (!up(InterfaceType::VIRTUAL, name, iface, ICanController::Result::OK)) GTEST_SKIP();
    assertRegistered(name, iface, true);

    const auto dnresult = mCanController->downInterface(name);
    ASSERT_TRUE(dnresult);
    assertRegistered(name, iface, false);
}

TEST_P(CanControllerHalTest, DownFake) {
    const auto result = mCanController->downInterface("imnotup");
    ASSERT_FALSE(result);
}

TEST_P(CanControllerHalTest, UpTwice) {
    const std::string name = mBusNames[0];
    const std::string iface = "vcan72";

    assertRegistered(name, iface, false);
    if (!up(InterfaceType::VIRTUAL, name, iface, ICanController::Result::OK)) GTEST_SKIP();
    assertRegistered(name, iface, true);
    if (!up(InterfaceType::VIRTUAL, name, "vcan73", ICanController::Result::INVALID_STATE)) {
        GTEST_SKIP();
    }
    assertRegistered(name, iface, true);

    const auto result = mCanController->downInterface(name);
    ASSERT_TRUE(result);
    assertRegistered(name, iface, false);
}

TEST_P(CanControllerHalTest, ConfigCompatibility) {
    // using random-ish addresses, which may not be valid - we can't test the success case
    // TODO(b/146214370): move interfaceId constructors to a library
    IfId virtualCfg = {};
    virtualCfg.virtualif({"vcan70"});

    IfId::Socketcan socketcanIfname = {};
    socketcanIfname.ifname("can0");
    IfId socketcanIfnameCfg = {};
    socketcanIfnameCfg.socketcan(socketcanIfname);

    IfId::Socketcan socketcanSerial = {};
    socketcanSerial.serialno({"1234", "2345"});
    IfId socketcanSerialCfg = {};
    socketcanSerialCfg.socketcan(socketcanSerial);

    IfId::Slcan slcanTtyname = {};
    slcanTtyname.ttyname("/dev/ttyUSB0");
    IfId slcanTtynameCfg = {};
    slcanTtynameCfg.slcan(slcanTtyname);

    IfId::Slcan slcanSerial = {};
    slcanSerial.serialno({"dead", "beef"});
    IfId slcanSerialCfg = {};
    slcanSerialCfg.slcan(slcanSerial);

    IfId indexedCfg = {};
    indexedCfg.indexed({0});

    static const std::vector<std::pair<InterfaceType, IfId>> compatMatrix = {
            {InterfaceType::VIRTUAL, virtualCfg},
            {InterfaceType::SOCKETCAN, socketcanIfnameCfg},
            {InterfaceType::SOCKETCAN, socketcanSerialCfg},
            {InterfaceType::SLCAN, slcanTtynameCfg},
            {InterfaceType::SLCAN, slcanSerialCfg},
            {InterfaceType::INDEXED, indexedCfg},
    };

    for (const auto [iftype, cfg] : compatMatrix) {
        LOG(INFO) << "Compatibility testing: " << iftype << " / " << cfg;

        ICanController::BusConfig config = {};
        config.name = "compattestsrv";
        config.bitrate = 125000;
        config.interfaceId = cfg;

        const auto upresult = mCanController->upInterface(config);

        if (!isSupported(iftype)) {
            ASSERT_EQ(ICanController::Result::NOT_SUPPORTED, upresult);
            continue;
        }
        ASSERT_NE(ICanController::Result::NOT_SUPPORTED, upresult);

        if (upresult == ICanController::Result::OK) {
            const auto dnresult = mCanController->downInterface(config.name);
            ASSERT_TRUE(dnresult);
            continue;
        }
    }
}

TEST_P(CanControllerHalTest, FailEmptyName) {
    const std::string name = "";
    const std::string iface = "vcan57";

    assertRegistered(name, iface, false);
    if (!up(InterfaceType::VIRTUAL, name, iface, ICanController::Result::BAD_SERVICE_NAME)) {
        GTEST_SKIP();
    }
    assertRegistered(name, iface, false);
}

TEST_P(CanControllerHalTest, FailBadName) {
    // 33 characters (name can be at most 32 characters long)
    const std::string name = "ab012345678901234567890123456789c";
    const std::string iface = "vcan57";

    assertRegistered(name, iface, false);
    if (!up(InterfaceType::VIRTUAL, name, iface, ICanController::Result::BAD_SERVICE_NAME)) {
        GTEST_SKIP();
    }
    assertRegistered(name, iface, false);
}

TEST_P(CanControllerHalTest, FailBadVirtualAddress) {
    const std::string name = mBusNames[0];
    const std::string iface = "";

    assertRegistered(name, iface, false);
    if (!up(InterfaceType::VIRTUAL, name, iface, ICanController::Result::BAD_INTERFACE_ID)) {
        GTEST_SKIP();
    }
    assertRegistered(name, iface, false);
}

TEST_P(CanControllerHalTest, FailBadSocketcanAddress) {
    const std::string name = mBusNames[0];
    const std::string iface = "can87";

    assertRegistered(name, iface, false);
    if (!up(InterfaceType::SOCKETCAN, name, iface, ICanController::Result::BAD_INTERFACE_ID)) {
        GTEST_SKIP();
    }
    assertRegistered(name, iface, false);

    auto supported =
            up(InterfaceType::SOCKETCAN, name, "", ICanController::Result::BAD_INTERFACE_ID);
    ASSERT_TRUE(supported);
    assertRegistered(name, iface, false);
}

TEST_P(CanControllerHalTest, FailBadSlcanAddress) {
    const std::string name = mBusNames[0];
    const std::string iface = "/dev/shouldnotexist123";

    assertRegistered(name, iface, false);
    if (!up(InterfaceType::SLCAN, name, iface, ICanController::Result::BAD_INTERFACE_ID)) {
        GTEST_SKIP();
    }
    assertRegistered(name, iface, false);

    auto supported = up(InterfaceType::SLCAN, name, "", ICanController::Result::BAD_INTERFACE_ID);
    ASSERT_TRUE(supported);
    assertRegistered(name, iface, false);
}

/**
 * Example manual invocation:
 * adb shell /data/nativetest64/VtsHalCanControllerV1_0TargetTest/VtsHalCanControllerV1_0TargetTest
 */
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(CanControllerHalTest);
INSTANTIATE_TEST_SUITE_P(PerInstance, CanControllerHalTest,
                         testing::ValuesIn(getAllHalInstanceNames(ICanController::descriptor)),
                         PrintInstanceNameToString);

}  // namespace android::hardware::automotive::can::V1_0::vts
