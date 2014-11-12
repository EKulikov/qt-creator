/**************************************************************************
**
** Copyright (c) 2014 BogDan Vatra <bog_dan_ro@yahoo.com>
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://www.qt.io/licensing.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#ifndef ANDROIDCONFIGURATIONS_H
#define ANDROIDCONFIGURATIONS_H

#include "android_global.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <QMap>
#include <QFutureInterface>

#include <projectexplorer/abi.h>

#include <utils/fileutils.h>
#include <utils/environment.h>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace ProjectExplorer { class Project; }

namespace Android {
class AndroidPlugin;

struct AndroidDeviceInfo
{
    QString serialNumber;
    QStringList cpuAbi;
    int sdk;
    enum State { OkState, UnAuthorizedState, OfflineState };
    State state;
    bool unauthorized;
    enum AndroidDeviceType { Hardware, Emulator };
    AndroidDeviceType type;

    static QStringList adbSelector(const QString &serialNumber);
};

class SdkPlatform
{
public:
    SdkPlatform()
        : apiLevel(-1)
    {}
    int apiLevel;
    QString name;
    QStringList abis;
};

class ANDROID_EXPORT AndroidConfig
{
public:
    AndroidConfig();

    void load(const QSettings &settings);
    void save(QSettings &settings) const;

    static QStringList apiLevelNamesFor(const QList<SdkPlatform> &platforms);
    static QString apiLevelNameFor(const SdkPlatform &platform);
    QList<SdkPlatform> sdkTargets(int minApiLevel = 0) const;

    Utils::FileName sdkLocation() const;
    void setSdkLocation(const Utils::FileName &sdkLocation);

    Utils::FileName ndkLocation() const;
    void setNdkLocation(const Utils::FileName &ndkLocation);

    Utils::FileName antLocation() const;
    void setAntLocation(const Utils::FileName &antLocation);

    Utils::FileName openJDKLocation() const;
    void setOpenJDKLocation(const Utils::FileName &openJDKLocation);

    Utils::FileName keystoreLocation() const;
    void setKeystoreLocation(const Utils::FileName &keystoreLocation);

    QString toolchainHost() const;
    QStringList makeExtraSearchDirectories() const;

    unsigned partitionSize() const;
    void setPartitionSize(unsigned partitionSize);

    bool automaticKitCreation() const;
    void setAutomaticKitCreation(bool b);

    bool useGrandle() const;
    void setUseGradle(bool b);

    Utils::FileName adbToolPath() const;
    Utils::FileName androidToolPath() const;
    Utils::Environment androidToolEnvironment() const;
    Utils::FileName antToolPath() const;
    Utils::FileName emulatorToolPath() const;


    Utils::FileName gccPath(ProjectExplorer::Abi::Architecture architecture, const QString &ndkToolChainVersion) const;
    Utils::FileName gdbPath(ProjectExplorer::Abi::Architecture architecture, const QString &ndkToolChainVersion) const;

    Utils::FileName keytoolPath() const;

    class CreateAvdInfo
    {
    public:
        QString target;
        QString name;
        QString abi;
        int sdcardSize;
        QString error; // only used in the return value of createAVD
    };

    CreateAvdInfo gatherCreateAVDInfo(QWidget *parent, int minApiLevel = 0, QString targetArch = QString()) const;
    QFuture<CreateAvdInfo> createAVD(CreateAvdInfo info) const;
    bool removeAVD(const QString &name) const;

    QVector<AndroidDeviceInfo> connectedDevices(QString *error = 0) const;
    QVector<AndroidDeviceInfo> androidVirtualDevices() const;
    QString startAVD(const QString &name, int apiLevel, QString cpuAbi) const;
    bool startAVDAsync(const QString &avdName) const;
    QString findAvd(int apiLevel, const QString &cpuAbi) const;
    QString waitForAvd(int apiLevel, const QString &cpuAbi, const QFutureInterface<bool> &fi = QFutureInterface<bool>()) const;
    QString bestNdkPlatformMatch(int target) const;

    static ProjectExplorer::Abi::Architecture architectureForToolChainPrefix(const QString &toolchainprefix);
    static QLatin1String toolchainPrefix(ProjectExplorer::Abi::Architecture architecture);
    static QLatin1String toolsPrefix(ProjectExplorer::Abi::Architecture architecture);

    QString getProductModel(const QString &device) const;
    bool hasFinishedBooting(const QString &device) const;
    bool waitForBooted(const QString &serialNumber, const QFutureInterface<bool> &fi) const;
    bool isConnected(const QString &serialNumber) const;

    SdkPlatform highestAndroidSdk() const;
private:
    static CreateAvdInfo createAVDImpl(CreateAvdInfo info, Utils::FileName androidToolPath, Utils::Environment env);

    Utils::FileName toolPath(ProjectExplorer::Abi::Architecture architecture, const QString &ndkToolChainVersion) const;
    Utils::FileName openJDKBinPath() const;
    int getSDKVersion(const QString &device) const;
    QStringList getAbis(const QString &device) const;
    bool isBootToQt(const QString &device) const;

    void updateAvailableSdkPlatforms() const;
    void updateNdkInformation() const;

    Utils::FileName m_sdkLocation;
    Utils::FileName m_ndkLocation;
    Utils::FileName m_antLocation;
    Utils::FileName m_openJDKLocation;
    Utils::FileName m_keystoreLocation;
    QStringList m_makeExtraSearchDirectories;
    unsigned m_partitionSize;
    bool m_automaticKitCreation;
    bool m_useGradle;

    //caches
    mutable bool m_availableSdkPlatformsUpToDate;
    mutable QVector<SdkPlatform> m_availableSdkPlatforms;
    static bool sortSdkPlatformByApiLevel(const SdkPlatform &a, const SdkPlatform &b);

    mutable bool m_NdkInformationUpToDate;
    mutable QString m_toolchainHost;
    mutable QVector<int> m_availableNdkPlatforms;

    mutable QHash<QString, QString> m_serialNumberToDeviceName;
};

class ANDROID_EXPORT AndroidConfigurations : public QObject
{
    friend class Android::AndroidPlugin;
    Q_OBJECT

public:
    static const AndroidConfig &currentConfig();
    static void setConfig(const AndroidConfig &config);
    static AndroidConfigurations *instance();

    static void updateAndroidDevice();
    static AndroidDeviceInfo showDeviceDialog(ProjectExplorer::Project *project, int apiLevel, const QString &abi);
    static void setDefaultDevice(ProjectExplorer::Project *project, const QString &abi, const QString &serialNumber); // serial number or avd name
    static QString defaultDevice(ProjectExplorer::Project *project, const QString &abi); // serial number or avd name
public slots:
    static void clearDefaultDevices(ProjectExplorer::Project *project);
    static void updateToolChainList();
    static void updateAutomaticKitList();
    static bool force32bitEmulator();

signals:
    void updated();

private:
    AndroidConfigurations(QObject *parent);
    void load();
    void save();

    static AndroidConfigurations *m_instance;
    AndroidConfig m_config;

    QMap<ProjectExplorer::Project *, QMap<QString, QString> > m_defaultDeviceForAbi;
    bool m_force32bit;
};

} // namespace Android

#endif // ANDROIDCONFIGURATIONS_H
