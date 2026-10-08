#include "ota.h"
#include "zip.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QFileInfo>
#include <QDir>
#include <QLibrary>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVersionNumber>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

#ifndef BOOKORBIT_VERSION
#define BOOKORBIT_VERSION "1.1.0"
#endif
#ifndef BOOKORBIT_UPDATE_PUBLIC_KEY
#define BOOKORBIT_UPDATE_PUBLIC_KEY ""
#endif

namespace Ota {
static bool fail(QString *error, const QString &text) { if(error) *error=text; return false; }
QString version() { return QStringLiteral(BOOKORBIT_VERSION); }
const char *buildInfo() {
    return "BOOKORBIT_BUILD_INFO:{\"version\":\"" BOOKORBIT_VERSION "\",\"publicKey\":\"" BOOKORBIT_UPDATE_PUBLIC_KEY "\",\"schema\":3}";
}
bool validVersion(const QString &v) {
    static const QRegularExpression re("^(0|[1-9][0-9]{0,5})\\.(0|[1-9][0-9]{0,5})\\.(0|[1-9][0-9]{0,5})$");
    const auto match=re.match(v); return match.hasMatch() && match.capturedLength()==v.size();
}
bool newer(const QString &a, const QString &b) {
    return validVersion(a) && validVersion(b) && QVersionNumber::compare(QVersionNumber::fromString(a),QVersionNumber::fromString(b))>0;
}
QByteArray publicKey() { return QByteArray::fromHex(BOOKORBIT_UPDATE_PUBLIC_KEY); }
QByteArray read(const QString &path, qint64 limit) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size()>limit || info.canonicalFilePath()!=info.absoluteFilePath()) return {};
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.read(limit+1) : QByteArray{};
}
QString hashFile(const QString &path) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.canonicalFilePath()!=info.absoluteFilePath()) return {};
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
bool safeDirectory(const QString &path) {
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && info.canonicalFilePath()==info.absoluteFilePath();
}
bool isRunningExecutable(const QString &path) {
#ifdef Q_OS_LINUX
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && !info.isSymLink() &&
        info.canonicalFilePath()==path && QCoreApplication::applicationFilePath()==path &&
        QFileInfo("/proc/self/exe").canonicalFilePath()==path;
#else
    Q_UNUSED(path)
    return false;
#endif
}
static bool commit(QSaveFile &file, QString *error) {
    if(!file.flush()) return fail(error,"flush: "+file.errorString());
    // QSaveFile::commit() ignores syncToDisk() failures; check before the rename.
    if(::fsync(file.handle())!=0) return fail(error,"sync_file: "+QString::fromLocal8Bit(std::strerror(errno)));
    const int dir=::open(QFile::encodeName(QFileInfo(file.fileName()).absolutePath()),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(dir<0) return fail(error,"open_directory: "+QString::fromLocal8Bit(std::strerror(errno)));
    if(!file.commit()) { ::close(dir); return fail(error,"commit: "+file.errorString()); }
    const bool ok=::fsync(dir)==0;
    const QString detail=ok ? QString{} : QString::fromLocal8Bit(std::strerror(errno));
    ::close(dir);
    return ok || fail(error,"sync_directory: "+detail);
}
bool save(const QString &path, const QByteArray &data, QString *error) {
    const QFileInfo info(path);
    if (!safeDirectory(info.absolutePath()) || info.isSymLink()) return fail(error,"Unsafe update path");
    QSaveFile f(path); f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly) || f.write(data)!=data.size()) return fail(error,f.errorString());
    return commit(f,error);
}
bool saveJson(const QString &path, const QJsonObject &data, QString *error) {
    return save(path,QJsonDocument(data).toJson(QJsonDocument::Compact),error);
}

// Use the firmware's OpenSSL 3 EVP ABI. No private Qt SSL API or bundled crypto.
struct evp_pkey_st; struct evp_md_ctx_st; struct evp_md_st; struct evp_pkey_ctx_st; struct engine_st;
bool verifyManifest(const QByteArray &data, const QByteArray &sig,QJsonObject *out,QString *error) {
    const QByteArray key=publicKey();
    if (key.size()!=32 || sig.size()!=64 || data.isEmpty() || data.size()>65536) return fail(error,"Missing or invalid update signature");
    QLibrary crypto("crypto",3);
    const auto nid=reinterpret_cast<int(*)(const char*)>(crypto.resolve("OBJ_txt2nid"));
    const auto makeKey=reinterpret_cast<evp_pkey_st*(*)(int,engine_st*,const unsigned char*,size_t)>(crypto.resolve("EVP_PKEY_new_raw_public_key"));
    const auto freeKey=reinterpret_cast<void(*)(evp_pkey_st*)>(crypto.resolve("EVP_PKEY_free"));
    const auto makeCtx=reinterpret_cast<evp_md_ctx_st*(*)()>(crypto.resolve("EVP_MD_CTX_new"));
    const auto freeCtx=reinterpret_cast<void(*)(evp_md_ctx_st*)>(crypto.resolve("EVP_MD_CTX_free"));
    const auto init=reinterpret_cast<int(*)(evp_md_ctx_st*,evp_pkey_ctx_st**,const evp_md_st*,engine_st*,evp_pkey_st*)>(crypto.resolve("EVP_DigestVerifyInit"));
    const auto verify=reinterpret_cast<int(*)(evp_md_ctx_st*,const unsigned char*,size_t,const unsigned char*,size_t)>(crypto.resolve("EVP_DigestVerify"));
    if(!nid||!makeKey||!freeKey||!makeCtx||!freeCtx||!init||!verify) return fail(error,"OpenSSL Ed25519 unavailable");
    auto *pk=makeKey(nid("ED25519"),nullptr,reinterpret_cast<const unsigned char*>(key.constData()),key.size());
    auto *ctx=makeCtx();
    const bool valid=pk && ctx && init(ctx,nullptr,nullptr,nullptr,pk)==1 &&
        verify(ctx,reinterpret_cast<const unsigned char*>(sig.constData()),sig.size(),reinterpret_cast<const unsigned char*>(data.constData()),data.size())==1;
    if(ctx) freeCtx(ctx);
    if(pk) freeKey(pk);
    if(!valid) return fail(error,"Update signature does not match");
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(data,&parse); const auto m=document.object();
    static const QRegularExpression sha("\\A[a-f0-9]{64}\\z");
    if(parse.error!=QJsonParseError::NoError || !document.isObject() || m["schema"]!=3 ||
       m["product"]!="bookorbit-pocketbook" || !validVersion(m["version"].toString()) ||
       m["tag"].toString()!="v"+m["version"].toString() || m["target"]!="PB634" ||
       m["firmware"]!="U634.6.10.3425" || m["abi"]!="armv7-softfp" ||
       m["layout"]!="single-app" || m["dataFormat"]!=1 ||
       !sha.match(m["sha256"].toString()).hasMatch() || m["bytes"].toDouble()<52 ||
       m["bytes"].toDouble()>maxClient || m["bytes"].toDouble()!=m["bytes"].toInteger()) return fail(error,"Incompatible update manifest");
    *out=m; return true;
}
static bool elf(const QString &path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return false;
    const auto b=f.read(52);
    return b.size()==52 && b.startsWith(QByteArray("\177ELF",4)) && b[4]==1 && b[5]==1 &&
        u16(b,18)==40 && (u32(b,36)&0x400)==0; // Reject hard-float ELF ABI.
}
bool stageArchive(const QString &archive,const QString &destination,const QString &expected,QJsonObject *manifest,QString *error) {
    Zip zip(archive); QJsonObject m;
    if(!verifyManifest(zip.read("release.json",65536),zip.read("release.sig",64),&m,error)) return false;
    if(m["version"].toString()!=expected) return fail(error,"Release and package do not match");
    if(!safeDirectory(QFileInfo(destination).absolutePath()) || QFileInfo(destination).isSymLink()) return fail(error,"Unsafe update path");
    if(!zip.extract("bookorbit.app",destination,m["bytes"].toInteger(),error)) return false;
    if(hashFile(destination)!=m["sha256"].toString() || !elf(destination)) {
        QFile::remove(destination); return fail(error,"Downloaded executable does not match");
    }
    *manifest=m; return true;
}
bool install(const QString &staged,const QString &destination,const QJsonObject &m,QString *error) {
    const QFileInfo target(destination), source(staged);
    if(staged==destination) return fail(error,"install.paths: source equals destination");
    if(!safeDirectory(target.absolutePath()) || !safeDirectory(source.absolutePath())) return fail(error,"install.paths: unsafe or missing directory");
    if(target.isSymLink() || !target.isFile()) return fail(error,"install.destination: missing file or symlink: "+destination);
    if(source.isSymLink() || !source.isFile()) return fail(error,"install.source: missing file or symlink");
    if(source.size()!=m["bytes"].toInteger()) return fail(error,"install.size: staged executable size differs");
    if(hashFile(staged)!=m["sha256"].toString()) return fail(error,"install.hash: staged executable checksum differs");
    if(!elf(staged)) return fail(error,"install.abi: incompatible executable");
    // QSaveFile writes beside the installed executable and atomically replaces it.
    // Never truncate a running executable, and never retain a previous version.
    QFile input(staged); QSaveFile output(destination); output.setDirectWriteFallback(false);
    if(!input.open(QIODevice::ReadOnly)) return fail(error,"install.open_source: "+input.errorString());
    if(!output.open(QIODevice::WriteOnly)) return fail(error,"install.open_destination: "+output.errorString());
    while(!input.atEnd()) {
        const auto chunk=input.read(65536);
        if(chunk.isEmpty()) return fail(error,"install.read: "+input.errorString());
        if(output.write(chunk)!=chunk.size()) return fail(error,"install.write: "+output.errorString());
    }
    // FAT exposes mount-defined execute bits and may reject chmod(0755).
    // Inspect the temporary file descriptor, not the still-installed pathname.
    struct stat mode{};
    if(::fstat(output.handle(),&mode)!=0) return fail(error,"install.permissions: cannot inspect temporary file");
    if(!(mode.st_mode&S_IXUSR)) {
        if(!output.setPermissions(output.permissions()|QFileDevice::ExeOwner))
            return fail(error,"install.permissions: "+output.errorString());
        if(::fstat(output.handle(),&mode)!=0 || !(mode.st_mode&S_IXUSR))
            return fail(error,"install.permissions: temporary file is not executable");
    }
    QString detail;
    if(!commit(output,&detail)) return fail(error,"install."+detail);
    if(!QFile::remove(staged)) return fail(error,"install.cleanup: cannot remove staged executable");
    return true;
}
}
