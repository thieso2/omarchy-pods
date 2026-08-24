#include "advmonitor.h"
#include "../BluetoothMonitor.h" // ManagedObjectList
#include "../logger.h"

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QTimer>

// One org.bluez.AdvertisementMonitor1 pattern: (start_position, ad_data_type, content).
struct AdvPattern
{
    quint8 startPosition = 0;
    quint8 adDataType = 0;
    QByteArray content;
};
Q_DECLARE_METATYPE(AdvPattern)
using AdvPatternList = QList<AdvPattern>;
Q_DECLARE_METATYPE(AdvPatternList)

// BlueZ's Device1.ManufacturerData dict, keyed by company id (a{qv}).
// QDBusVariant, not QVariant: the generic map marshaller has no
// operator for a bare QVariant value.
using ManufacturerDataMap = QMap<quint16, QDBusVariant>;
Q_DECLARE_METATYPE(ManufacturerDataMap)

QDBusArgument &operator<<(QDBusArgument &arg, const AdvPattern &pattern)
{
    arg.beginStructure();
    arg << pattern.startPosition << pattern.adDataType << pattern.content;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, AdvPattern &pattern)
{
    arg.beginStructure();
    arg >> pattern.startPosition >> pattern.adDataType >> pattern.content;
    arg.endStructure();
    return arg;
}

namespace
{

constexpr quint16 appleCompanyId = 0x004C;
// AD type 0xFF is manufacturer-specific data; its payload opens with the
// company id little-endian, so Apple proximity-pairing frames begin
// 4C 00 07 and this pattern admits nothing else.
constexpr quint8 manufacturerDataAdType = 0xFF;

const QString bluezService = QStringLiteral("org.bluez");
const QString propertiesIface = QStringLiteral("org.freedesktop.DBus.Properties");
const QString deviceIface = QStringLiteral("org.bluez.Device1");
const QString monitorManagerIface = QStringLiteral("org.bluez.AdvertisementMonitorManager1");
const QString monitorIface = QStringLiteral("org.bluez.AdvertisementMonitor1");
// bluetoothd reads our monitor through an ObjectManager rooted here.
const QString appRootPath = QStringLiteral("/org/openpods/advmon");
const QString monitorPath = QStringLiteral("/org/openpods/advmon/apple");

AdvPatternList applePatterns()
{
    AdvPattern pattern;
    pattern.startPosition = 0;
    pattern.adDataType = manufacturerDataAdType;
    pattern.content = QByteArray::fromHex("4c0007");
    return {pattern};
}

QVariantMap monitorProperties()
{
    QVariantMap props;
    props.insert(QStringLiteral("Type"), QStringLiteral("or_patterns"));
    props.insert(QStringLiteral("Patterns"), QVariant::fromValue(applePatterns()));
    return props;
}

// Sample input: /org/bluez/hci0/dev_58_1E_FB_67_34_45 — BlueZ encodes the address in the path.
QString addressFromDevicePath(const QString &path)
{
    const QString tail = path.section(QStringLiteral("/dev_"), 1, 1);
    return QString(tail).replace(QLatin1Char('_'), QLatin1Char(':'));
}

// Returns Apple's entry from a marshalled ManufacturerData dict, empty when absent.
QByteArray appleDataFrom(const QVariant &manufacturerData)
{
    if (!manufacturerData.canConvert<QDBusArgument>())
        return QByteArray();
    ManufacturerDataMap byCompany;
    manufacturerData.value<QDBusArgument>() >> byCompany;
    return byCompany.value(appleCompanyId).variant().toByteArray();
}

} // namespace

// The org.freedesktop.DBus.ObjectManager bluetoothd walks on RegisterMonitor.
class AdvMonitorObjectManagerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.DBus.ObjectManager")
public:
    explicit AdvMonitorObjectManagerAdaptor(QObject *parent) : QDBusAbstractAdaptor(parent) {}

public slots:
    ManagedObjectList GetManagedObjects()
    {
        ManagedObjectList objects;
        QMap<QString, QVariantMap> interfaces;
        interfaces.insert(monitorIface, monitorProperties());
        objects.insert(QDBusObjectPath(monitorPath), interfaces);
        return objects;
    }
};

class AdvMonitorAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.AdvertisementMonitor1")
    Q_PROPERTY(QString Type READ type)
    Q_PROPERTY(AdvPatternList Patterns READ patterns)
public:
    AdvMonitorAdaptor(QObject *object, AdvMonitor *owner)
        : QDBusAbstractAdaptor(object), m_owner(owner) {}

    QString type() const { return QStringLiteral("or_patterns"); }
    AdvPatternList patterns() const { return applePatterns(); }

public slots:
    void Activate() { m_owner->onActivated(); }
    void Release() { m_owner->onReleased(); }
    void DeviceFound(const QDBusObjectPath &device) { m_owner->onDeviceFound(device); }
    void DeviceLost(const QDBusObjectPath &device) { m_owner->onDeviceLost(device); }

private:
    AdvMonitor *m_owner;
};

AdvMonitor::AdvMonitor(QObject *parent) : QObject(parent)
{
    if (!m_bus.isConnected()) {
        LOG_WARN("AdvMonitor: system D-Bus unavailable");
        return;
    }

    // Re-register after a bluetoothd restart, which silently drops every monitor.
    auto *watcher = new QDBusServiceWatcher(bluezService, m_bus,
                                            QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered,
            this, &AdvMonitor::onBluezRegistered);
}

void AdvMonitor::ensureExported()
{
    if (m_exported)
        return;

    qDBusRegisterMetaType<AdvPattern>();
    qDBusRegisterMetaType<AdvPatternList>();
    qDBusRegisterMetaType<ManufacturerDataMap>();
    qDBusRegisterMetaType<ManagedObjectList>();

    new AdvMonitorObjectManagerAdaptor(this);
    m_monitorObject = new QObject(this);
    new AdvMonitorAdaptor(m_monitorObject, this);
    m_bus.registerObject(appRootPath, this, QDBusConnection::ExportAdaptors);
    m_bus.registerObject(monitorPath, m_monitorObject, QDBusConnection::ExportAdaptors);

    // Route the whole QDBusMessage so the slot sees the originating path
    // (mirrors BluetoothMonitor::registerDBusService, and the service
    // stays blank for the same reason: signals arrive from a unique name
    // we don't know up-front).
    m_bus.connect(QString(), QString(), propertiesIface, QStringLiteral("PropertiesChanged"),
                  this, SLOT(onPropertiesChanged(QDBusMessage)));

    m_exported = true;
}

bool AdvMonitor::start()
{
    m_wanted = true;
    m_releaseRetries = 0;
    if (m_active)
        return true;
    if (!m_bus.isConnected())
        return false;
    return registerMonitor();
}

void AdvMonitor::stop()
{
    m_wanted = false;
    if (!m_active)
        return;
    QDBusInterface manager(bluezService, m_adapterPath, monitorManagerIface, m_bus);
    manager.call(QStringLiteral("UnregisterMonitor"),
                 QVariant::fromValue(QDBusObjectPath(appRootPath)));
    m_active = false;
    m_aliasByPath.clear();
}

bool AdvMonitor::registerMonitor()
{
    ensureExported();

    // Find the adapter that offers the monitor manager (one box, but hci
    // numbering is not guaranteed to start at 0).
    QDBusInterface objectManager(bluezService, QStringLiteral("/"),
                                 QStringLiteral("org.freedesktop.DBus.ObjectManager"), m_bus);
    QDBusMessage reply = objectManager.call(QStringLiteral("GetManagedObjects"));
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        LOG_WARN("AdvMonitor: GetManagedObjects failed: " << reply.errorMessage());
        return false;
    }
    ManagedObjectList managedObjects;
    reply.arguments().constFirst().value<QDBusArgument>() >> managedObjects;

    QString adapterPath;
    for (auto it = managedObjects.constBegin(); it != managedObjects.constEnd(); ++it) {
        if (it.value().contains(monitorManagerIface)) {
            adapterPath = it.key().path();
            break;
        }
    }
    if (adapterPath.isEmpty()) {
        LOG_WARN("AdvMonitor: no adapter offers " << monitorManagerIface);
        return false;
    }

    QDBusInterface manager(bluezService, adapterPath, monitorManagerIface, m_bus);
    QDBusReply<void> registered = manager.call(QStringLiteral("RegisterMonitor"),
                                               QVariant::fromValue(QDBusObjectPath(appRootPath)));
    if (!registered.isValid()) {
        LOG_WARN("AdvMonitor: RegisterMonitor failed: " << registered.error().message());
        return false;
    }

    m_adapterPath = adapterPath;
    m_active = true;
    LOG_INFO("AdvMonitor: Apple advertisement monitor registered on " << adapterPath);
    return true;
}

void AdvMonitor::onActivated()
{
    m_releaseRetries = 0;
    LOG_DEBUG("AdvMonitor: monitor activated by bluetoothd");
}

void AdvMonitor::onReleased()
{
    m_active = false;
    if (!m_wanted)
        return;
    // One immediate rejection can be transient (adapter settling); two means unsupported.
    if (m_releaseRetries++ < 1) {
        QTimer::singleShot(2000, this, [this]() {
            if (m_wanted && !m_active && !registerMonitor())
                emit failed();
        });
        return;
    }
    LOG_WARN("AdvMonitor: bluetoothd released the monitor twice, giving up");
    emit failed();
}

void AdvMonitor::onDeviceFound(const QDBusObjectPath &device)
{
    const QString path = device.path();
    QDBusInterface props(bluezService, path, propertiesIface, m_bus);
    QDBusMessage reply = props.call(QStringLiteral("GetAll"), deviceIface);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
        return;

    const QVariantMap deviceProps = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
    m_aliasByPath.insert(path, deviceProps.value(QStringLiteral("Alias")).toString());
    emitFrame(path, appleDataFrom(deviceProps.value(QStringLiteral("ManufacturerData"))));
}

void AdvMonitor::onDeviceLost(const QDBusObjectPath &device)
{
    m_aliasByPath.remove(device.path());
}

void AdvMonitor::onPropertiesChanged(const QDBusMessage &message)
{
    // Only matched devices reach the bus while no discovery runs, but
    // filter anyway: the blank-service subscription sees every Properties
    // signal on the system bus.
    if (!m_active || !message.path().startsWith(QStringLiteral("/org/bluez/")))
        return;

    const QList<QVariant> args = message.arguments();
    if (args.size() < 2 || args.at(0).toString() != deviceIface)
        return;

    const QVariantMap changed = qdbus_cast<QVariantMap>(args.at(1));
    if (!changed.contains(QStringLiteral("ManufacturerData")))
        return;

    emitFrame(message.path(), appleDataFrom(changed.value(QStringLiteral("ManufacturerData"))));
}

void AdvMonitor::emitFrame(const QString &devicePath, const QByteArray &appleData)
{
    if (appleData.isEmpty())
        return;
    emit advertisement(addressFromDevicePath(devicePath), m_aliasByPath.value(devicePath), appleData);
}

void AdvMonitor::onBluezRegistered()
{
    if (!m_wanted || m_active)
        return;
    // Fresh bluetoothd needs a moment to expose its adapters.
    QTimer::singleShot(1000, this, [this]() {
        if (m_wanted && !m_active && !registerMonitor())
            emit failed();
    });
}

#include "advmonitor.moc"
