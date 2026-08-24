#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QString>

class QDBusMessage;

// A BlueZ org.bluez.AdvertisementMonitor1 client matching Apple
// proximity-pairing frames (manufacturer data starting 4C 00 07). BlueZ
// filters in the kernel (or in bluetoothd where the controller lacks
// offload), so no discovery session runs and the system bus never sees
// the RSSI chatter of every BLE device in range — an unfiltered scan in
// a dense environment was measured driving wireplumber alone to 70% CPU.
class AdvMonitor : public QObject
{
    Q_OBJECT
public:
    explicit AdvMonitor(QObject *parent = nullptr);

    bool start();
    void stop();
    bool isActive() const { return m_active; }

    // Entry points for the D-Bus adaptors defined in advmonitor.cpp; bluetoothd is the caller.
    void onActivated();
    void onReleased();
    void onDeviceFound(const QDBusObjectPath &device);
    void onDeviceLost(const QDBusObjectPath &device);

signals:
    // One Apple manufacturer-data frame (company id stripped), address being the advertiser's current RPA.
    void advertisement(const QString &address, const QString &name, const QByteArray &appleData);
    // The monitor could not be established or re-established; the caller may fall back to a discovery scan.
    void failed();
    // The monitor is up — possibly late, after start() already returned
    // false on a lost D-Bus reply; the caller can stop any fallback scan.
    void established();

private slots:
    void onPropertiesChanged(const QDBusMessage &message);
    void onBluezRegistered();

private:
    void ensureExported();
    bool registerMonitor();
    void establish();
    void beginPendingActivation();
    void emitFrame(const QString &devicePath, const QByteArray &appleData);

    QDBusConnection m_bus = QDBusConnection::systemBus();
    QObject *m_monitorObject = nullptr;
    QString m_adapterPath;
    // Alias per device path, captured on DeviceFound so ManufacturerData
    // updates (which carry no name) can still label the device.
    QHash<QString, QString> m_aliasByPath;
    bool m_exported = false;
    bool m_wanted = false;
    bool m_active = false;
    // RegisterMonitor's reply was lost; bluetoothd may still hold the
    // registration, and its Activate() call is what settles it.
    bool m_pendingActivate = false;
    int m_releaseRetries = 0;
    int m_registerRetries = 0;
};
