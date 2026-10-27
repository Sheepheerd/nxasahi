// nn::bluetooth (Bluetooth LE, through the btm and bt services): Pokémon: Let's Go connects to them at boot for the Poké Ball Plus. The host has no Bluetooth: Initialize succeeds,
// a scan finds nothing, no device is ever connected, and the events are bound to kernel events that never fire. The GATT object classes (GattService, GattCharacteristic,
// GattDescriptor: constructors, SetValue, the getters) are plain data and run as they are; only what reaches the services is taken over. Without this the SDK's own
// nn::btm::user::InitializeBtmUserInterface aborted on the host's stand-in btm:u session (its GetCore returns an object the stand-in cannot give)
#include "hle.h"

#define BT_OK(sym, what, fn, ...) HLE_FORCE(sym, what, Result, fn, __VA_ARGS__)
#define BT_EVENT(sym, what, fn) HLE_FORCE(sym, what ": bound, never fires", Result, fn, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }

HLE_FORCE("_ZN2nn9bluetooth13InitializeBleEv", "Bluetooth LE: initialized (no adapter behind it)", Result, h_bt_InitBle, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3btm4user26InitializeBtmUserInterfaceEv", "Bluetooth manager client: initialized (no adapter behind it)", void, h_btm_InitUser, void) {}

BT_EVENT("_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE", "BLE scan event", h_bt_ScanEv)
BT_EVENT("_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE", "BLE MTU event", h_bt_MtuEv)
BT_EVENT("_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE", "BLE GATT operation event", h_bt_GattEv)
BT_EVENT("_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE", "BLE service discovery event", h_bt_DiscEv)
BT_EVENT("_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE", "BLE connection state event", h_bt_ConnEv)

BT_OK("_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE", "BLE scan for a smart device: started, finds nothing", h_bt_StartScan, const void *uuid) { (void)uuid; return RES_OK; }
BT_OK("_ZN2nn9bluetooth22StopBleScanSmartDeviceEv", "BLE scan: stopped", h_bt_StopScan, void) { return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi", "BLE scan results: none", int, h_bt_ScanRes, void *out, int n) { (void)out; (void)n; return 0; }
BT_OK("_ZN2nn9bluetooth19GetBleScanParameterEPNS0_17GattAttributeUuidEi", "BLE scan parameter: an empty UUID", h_bt_ScanParam, void *uuid, int which) {
    (void)which; memset(uuid, 0, 20); return RES_OK;   // GattAttributeUuid: length + 16 bytes
}
HLE_FORCE("_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi", "BLE connections: none", int, h_bt_ConnList, void *out, int n) { (void)out; (void)n; return 0; }
BT_OK("_ZN2nn9bluetooth19ConnectToGattServerERKNS0_7AddressE", "BLE connect: refused (no adapter)", h_bt_Connect, const void *addr) { (void)addr; return RES_FAIL; }
BT_OK("_ZN2nn9bluetooth24DisconnectFromGattServerEj", "BLE disconnect", h_bt_Disconnect, unsigned conn) { (void)conn; return RES_OK; }
BT_OK("_ZN2nn9bluetooth15ConfigureBleMtuEjt", "BLE MTU", h_bt_Mtu, unsigned conn, unsigned mtu) { (void)conn; (void)mtu; return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth15GetGattServicesEPNS0_11GattServiceEij", "GATT services of a connection: none", int, h_bt_Services, void *out, int n, unsigned conn) { (void)out; (void)n; (void)conn; return 0; }
BT_OK("_ZN2nn9bluetooth22GetGattOperationResultEPNS0_26BleClientGattOperationInfoE", "GATT operation result: none pending", h_bt_OpRes, void *info) { (void)info; return RES_FAIL; }
BT_OK("_ZN2nn9bluetooth22ReadGattCharacteristicERKNS0_18GattCharacteristicE", "GATT read: refused (not connected)", h_bt_Read, const void *c) { (void)c; return RES_FAIL; }
BT_OK("_ZN2nn9bluetooth23WriteGattCharacteristicERKNS0_18GattCharacteristicE", "GATT write: refused (not connected)", h_bt_Write, const void *c) { (void)c; return RES_FAIL; }
BT_OK("_ZN2nn9bluetooth19WriteGattDescriptorERKNS0_14GattDescriptorE", "GATT descriptor write: refused (not connected)", h_bt_WriteDesc, const void *d) { (void)d; return RES_FAIL; }
BT_OK("_ZN2nn9bluetooth36EnableGattCharacteristicNotificationERKNS0_18GattCharacteristicEb", "GATT notification: refused (not connected)", h_bt_Notify, const void *c, int on) { (void)c; (void)on; return RES_FAIL; }
BT_OK("_ZN2nn9bluetooth33RegisterGattOperationNotificationERKNS0_17GattAttributeUuidE", "GATT operation notification: registered", h_bt_RegNotify, const void *uuid) { (void)uuid; return RES_OK; }
BT_OK("_ZN2nn9bluetooth35UnregisterGattOperationNotificationERKNS0_17GattAttributeUuidE", "GATT operation notification: unregistered", h_bt_UnregNotify, const void *uuid) { (void)uuid; return RES_OK; }
