#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class QNetworkAccessManager;

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        DetailsRole,
        WakeUrlRole // GFN fork
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);

    Q_INVOKABLE void testConnectionForComputer(int computerIndex);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    // GFN fork: "Start mit einem Tipp". A wake address is an http(s) URL (optionally prefixed
    // with "POST ") that powers on/wakes the PC, e.g. a smart plug, UpSnap or a Home Assistant
    // webhook. It is stored per PC (by UUID) in the settings.
    Q_INVOKABLE void setWakeUrl(int computerIndex, QString url);

    Q_INVOKABLE bool isComputerOnline(int computerIndex);

    Q_INVOKABLE bool isComputerPaired(int computerIndex);

    Q_INVOKABLE bool isComputerServerSupported(int computerIndex);

    // Calls the wake address (if any) and sends Wake-on-LAN (if the MAC address is known).
    // The result of the wake address arrives through wakeUrlCalled().
    Q_INVOKABLE void wakeAndConnect(int computerIndex);

signals:
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);
    void wakeUrlCalled(int computerIndex, bool success, QString message);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    static QString wakeUrlKey(const QString& uuid);

    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
    QNetworkAccessManager* m_WakeNam = nullptr;
};
