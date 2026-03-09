/*
 * @doc     docs/modules/ble-channel/design/01-ble-l2cap-hello-world-ios-design.md §2 §3
 * @purpose CoreBluetooth 管理器：
 *   - 扫描 VoiceMask-01（按 PSM Service UUID 过滤）
 *   - 连接 → 发现 GATT Service → 读取 PSM Characteristic → 建立 L2CAP 信道
 *   - 接收 ESP32 发来的数据，发布到 messages
 *   - 断连后自动重新扫描
 */

import Foundation
import CoreBluetooth

// MARK: - 常量

let PSM_SERVICE_UUID        = CBUUID(string: "0000AE00-0000-1000-8000-00805F9B34FB")
let PSM_CHARACTERISTIC_UUID = CBUUID(string: "0000AE01-0000-1000-8000-00805F9B34FB")

// MARK: - 数据模型

struct BLEMessage: Identifiable {
    let id = UUID()
    let content: String
    let timestamp: Date
}

enum ConnectionState {
    case unauthorized
    case scanning
    case connecting
    case connected(name: String)
    case disconnected

    var label: String {
        switch self {
        case .unauthorized:         return "请开启蓝牙权限"
        case .scanning:             return "扫描中..."
        case .connecting:           return "连接中..."
        case .connected(let name):  return "已连接 \(name)"
        case .disconnected:         return "已断连，重新扫描中"
        }
    }

    var color: ConnectionStateColor {
        switch self {
        case .unauthorized, .disconnected: return .red
        case .scanning, .connecting:       return .yellow
        case .connected:                   return .green
        }
    }
}

enum ConnectionStateColor { case red, yellow, green }

// MARK: - BLEManager

final class BLEManager: NSObject, ObservableObject {

    @Published var connectionState: ConnectionState = .scanning
    @Published var messages: [BLEMessage] = []

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    @Published private(set) var l2capHandler: L2CAPHandler?
    private var psmCharacteristic: CBCharacteristic?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    // MARK: - Private

    private func startScan() {
        guard central.state == .poweredOn else { return }
        connectionState = .scanning
        central.scanForPeripherals(withServices: [PSM_SERVICE_UUID])
    }

    private func cleanup() {
        l2capHandler?.close()
        l2capHandler = nil
        psmCharacteristic = nil
        if let p = peripheral {
            central.cancelPeripheralConnection(p)
        }
        peripheral = nil
    }

    // MARK: - Public

    func clearMessages() {
        messages.removeAll()
    }

    func appendMessage(_ content: String) {
        if messages.count >= 100 {
            messages.removeFirst()
        }
        messages.append(BLEMessage(content: content, timestamp: Date()))
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScan()
        case .unauthorized:
            connectionState = .unauthorized
        default:
            connectionState = .disconnected
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        guard self.peripheral == nil else { return }
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        connectionState = .connecting
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager,
                        didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([PSM_SERVICE_UUID])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        cleanup()
        startScan()
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        print("[BLE] didDisconnect: \(peripheral.name ?? "?"), error=\(error?.localizedDescription ?? "nil")")
        connectionState = .disconnected
        cleanup()
        startScan()
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: {
            $0.uuid == PSM_SERVICE_UUID
        }) else { return }
        peripheral.discoverCharacteristics([PSM_CHARACTERISTIC_UUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let char = service.characteristics?.first(where: {
            $0.uuid == PSM_CHARACTERISTIC_UUID
        }) else { return }
        psmCharacteristic = char
        peripheral.readValue(for: char)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == PSM_CHARACTERISTIC_UUID,
              let data = characteristic.value,
              data.count >= 2 else {
            // PSM 读取失败，断开重连
            cleanup()
            startScan()
            return
        }
        // 2 字节 Little-Endian 解码
        let psm = CBL2CAPPSM(data[0]) | (CBL2CAPPSM(data[1]) << 8)
        peripheral.openL2CAPChannel(psm)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didOpen channel: CBL2CAPChannel?,
                    error: Error?) {
        guard let channel = channel, error == nil else {
            cleanup()
            startScan()
            return
        }
        connectionState = .connected(name: peripheral.name ?? "VoiceMask")
        l2capHandler = L2CAPHandler(channel: channel, manager: self)
        l2capHandler?.open()
    }
}
