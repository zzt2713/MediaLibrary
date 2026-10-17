#include "MediaDatabase.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QVariant>
#include "Logger.h"

namespace media {

namespace {

/*!
 * schema 版本。只在索引集合发生变化时递增。
 *
 * v1：初始版本（folder / type / mtime / favorite / missing / hash 六个单列索引）
 * v2：新增四个组合索引（folder+mtime / type+mtime / name / size）
 *     旧库打开时由 ensureIndexes() 检测到版本落后并补建。
 */
constexpr int kSchemaVersion = 2;

/*! 把路径规范成统一形式，避免 "D:/a/./b" 和 "D:/a/b" 被当成两条记录。 */
QString normalizePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

MediaType typeFromInt(int v)
{
    switch (v) {
    case 1: return MediaType::Image;
    case 2: return MediaType::Video;
    case 3: return MediaType::Audio;
    default: return MediaType::Unknown;
    }
}

} // namespace

MediaDatabase::MediaDatabase(QObject *parent)
    : QObject(parent)
{
    m_baseConnectionName = QStringLiteral("media_lib_%1")
                               .arg(reinterpret_cast<quintptr>(this), 0, 16);
}

MediaDatabase::~MediaDatabase()
{
    close();
}

// ---------------------------------------------------------------- 连接管理

QString MediaDatabase::connectionForCurrentThread() const
{
    // 线程指针作为后缀就够唯一了：同一线程内所有调用共用一条连接，
    // 不同线程拿到不同连接名，互不干扰。
    const QString name = QStringLiteral("%1_t%2")
                             .arg(m_baseConnectionName)
                             .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);

    if (!QSqlDatabase::contains(name)) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(m_dbPath);
        if (!db.open()) {
            m_lastError = db.lastError().text();
            qWarning() << "[MediaDatabase] open failed:" << m_lastError;
        } else {
            applyPragmas(db);
        }
    }

    QMutexLocker locker(&m_connMutex);
    if (!m_connectionNames.contains(name))
        m_connectionNames.append(name);

    return name;
}

void MediaDatabase::applyPragmas(QSqlDatabase &db) const
{
    QSqlQuery q(db);
    // WAL 让读写并发不互锁，扫描线程写的同时 UI 线程还能读
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    // 降低 fsync 频率，扫描时快很多；异常退出最多丢最后一次事务
    q.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    q.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
}

bool MediaDatabase::open(const QString &dbPath)
{
    if (dbPath.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        m_dbPath = QDir(dir).filePath(QStringLiteral("library.db"));
    } else {
        m_dbPath = normalizePath(dbPath);
        QDir().mkpath(QFileInfo(m_dbPath).absolutePath());
    }

    const QString conn = connectionForCurrentThread();
    QSqlDatabase db = QSqlDatabase::database(conn);
    if (!db.isOpen()) {
        m_lastError = db.lastError().text();
        return false;
    }

    if (!ensureSchema())
        return false;

    return ensureIndexes();
}

void MediaDatabase::close()
{
    QMutexLocker locker(&m_connMutex);
    const QList<QString> names = m_connectionNames;
    m_connectionNames.clear();
    locker.unlock();

    for (const QString &name : names) {
        if (!QSqlDatabase::contains(name))
            continue;
        {
            // 必须先让 QSqlDatabase 对象出作用域，否则 removeDatabase 会警告连接仍在使用
            QSqlDatabase db = QSqlDatabase::database(name, false);
            if (db.isOpen()) {
                QSqlQuery q(db);
                q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
    }
}

bool MediaDatabase::isOpen() const
{
    const QString conn = connectionForCurrentThread();
    return QSqlDatabase::contains(conn) && QSqlDatabase::database(conn, false).isOpen();
}

bool MediaDatabase::ensureSchema()
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);

    bool ok = true;

    ok = q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS folders (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            path         TEXT NOT NULL UNIQUE,
            display_name TEXT,
            added_at     INTEGER NOT NULL,
            last_scan    INTEGER NOT NULL DEFAULT 0
        )
    )")) && ok;

    ok = q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS media_items (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            path         TEXT NOT NULL UNIQUE,
            folder_path  TEXT NOT NULL,
            file_name    TEXT NOT NULL,
            type         INTEGER NOT NULL,
            size         INTEGER NOT NULL DEFAULT 0,
            mtime        INTEGER NOT NULL DEFAULT 0,
            favorite     INTEGER NOT NULL DEFAULT 0,
            missing      INTEGER NOT NULL DEFAULT 0,
            content_hash TEXT,
            visual_hash  TEXT,
            width        INTEGER NOT NULL DEFAULT 0,
            height       INTEGER NOT NULL DEFAULT 0,
            duration_ms  REAL    NOT NULL DEFAULT -1,
            camera_make  TEXT,
            camera_model TEXT,
            taken_at     INTEGER,
            indexed_at   INTEGER NOT NULL
        )
    )")) && ok;

    /*!
     * 索引不在这里建。
     *
     * 本函数只负责表结构，索引发在与版本号联动的 ensureIndexes() 里。
     * 若把 CREATE INDEX 放在这条"每次 open() 都会执行"的路径上，
     * 旧库升级时追加的新索引会在每次打开时都被执行一遍——
     * 虽然 IF NOT EXISTS 让它们成为 no-op，但二十万条的库上，
     * SQLite 仍会为每条语句做目录查询与锁检查，白耗启动时间。
     */

    ok = q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS tags (
            id   INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE
        )
    )")) && ok;

    ok = q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS item_tags (
            item_id  INTEGER NOT NULL,
            tag_id   INTEGER NOT NULL,
            PRIMARY KEY (item_id, tag_id),
            FOREIGN KEY (item_id) REFERENCES media_items(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id)  REFERENCES tags(id)         ON DELETE CASCADE
        )
    )")) && ok;

    ok = q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS play_history (
            item_id     INTEGER PRIMARY KEY,
            last_play   INTEGER NOT NULL,
            position_ms REAL    NOT NULL DEFAULT 0,
            play_count  INTEGER NOT NULL DEFAULT 1,
            FOREIGN KEY (item_id) REFERENCES media_items(id) ON DELETE CASCADE
        )
    )")) && ok;

    ok = q.exec(QStringLiteral(R"(
        CREATE TABLE IF NOT EXISTS schema_meta (
            key   TEXT PRIMARY KEY,
            value TEXT
        )
    )")) && ok;

    if (!ok) {
        m_lastError = q.lastError().text();
        qWarning() << "[MediaDatabase] schema init failed:" << m_lastError;
        emit errorOccurred(m_lastError);
        return false;
    }

    return ok;
}

int MediaDatabase::storedSchemaVersion(QSqlDatabase &db) const
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT value FROM schema_meta WHERE key = 'version'"))) {
        return 0;
    }
    if (!q.next())
        return 0;
    return q.value(0).toInt();
}

bool MediaDatabase::ensureIndexes()
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    // 版本没变就不碰索引：旧库打开时这条路是常态（第二、三次启动）。
    // 只有第一次升级（0 → 当前版本）或全新库才需要建。
    const int stored = storedSchemaVersion(db);
    if (stored >= kSchemaVersion)
        return true;

    LOG_INFO("DB") << "building indexes: schema " << stored << " -> " << kSchemaVersion;

    /*!
     * 索引按实际会触发的视图逐条建立，全部经过 EXPLAIN QUERY PLAN
     * + 20 万条数据实测（脚本见 build/_idx_probe2.py）：
     *
     *  视图            缺失索引时                            有索引后
     *  按名称排序       2.6ms（全表 SCAN + TEMP B-TREE）      0.32ms
     *  按大小排序       2.7ms（全表 SCAN + TEMP B-TREE）      0.32ms
     *  按类型 + 时间    type 索引后再排一次序                  0.46ms
     *
     * 两个要点：
     *  1) 组合索引都带 mtime DESC —— mtime 是默认视图的排序键，
     *     这样"筛选 + 排序"一次走完，省掉 SQLite 的 TEMP B-TREE；
     *  2) file_name 必须写 COLLATE NOCASE —— 代理模型的 lessThan 用
     *     localeAwareCompare，不带 COLLATE 的索引对大小写不敏感排序无效。
     */
    QSqlQuery q(db);
    bool ok = true;

    // 单列索引
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_folder ON media_items(folder_path)")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_type ON media_items(type)")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_mtime ON media_items(mtime DESC)")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_favorite ON media_items(favorite) WHERE favorite = 1")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_missing ON media_items(missing) WHERE missing = 1")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_hash ON media_items(content_hash) WHERE content_hash IS NOT NULL")) && ok;

    // 组合索引：目录 / 类型 / 名称 / 大小，各自带上 mtime 供排序使用
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_folder_mtime "
        "ON media_items(folder_path, mtime DESC)")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_type_mtime "
        "ON media_items(type, mtime DESC)")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_name "
        "ON media_items(file_name COLLATE NOCASE)")) && ok;
    ok = q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_items_size ON media_items(size DESC)")) && ok;

    if (!ok) {
        m_lastError = q.lastError().text();
        qWarning() << "[MediaDatabase] index creation failed:" << m_lastError;
        emit errorOccurred(m_lastError);
        return false;
    }

    // 建完再写版本号：中途失败时版本仍停在旧值，下次启动会重试，
    // 而不是把一个"索引建了一半"的库标记成最新版。
    QSqlQuery vq(db);
    vq.prepare(QStringLiteral("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('version', ?)"));
    vq.addBindValue(QString::number(kSchemaVersion));
    if (!vq.exec()) {
        m_lastError = vq.lastError().text();
        qWarning() << "[MediaDatabase] schema version update failed:" << m_lastError;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------- 目录

qint64 MediaDatabase::addFolder(const QString &path, const QString &displayName)
{
    const QString norm = normalizePath(path);
    if (norm.isEmpty() || !QFileInfo(norm).isDir())
        return 0;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);

    q.prepare(QStringLiteral(
        "INSERT INTO folders(path, display_name, added_at, last_scan) "
        "VALUES(:p, :n, :t, :s) "
        "ON CONFLICT(path) DO UPDATE SET display_name = :n"));
    q.bindValue(QStringLiteral(":p"), norm);
    q.bindValue(QStringLiteral(":n"), displayName.isEmpty() ? QFileInfo(norm).fileName() : displayName);
    q.bindValue(QStringLiteral(":t"), QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":s"), QDateTime::currentMSecsSinceEpoch());

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return 0;
    }

    // 冲突分支不返回 lastInsertId，这里统一再查一次，保证两种情况都对
    QSqlQuery idq(db);
    idq.prepare(QStringLiteral("SELECT id FROM folders WHERE path = ?"));
    idq.addBindValue(norm);
    if (idq.exec() && idq.next())
        return idq.value(0).toLongLong();

    return 0;
}

void MediaDatabase::removeFolder(const QString &path)
{
    const QString norm = normalizePath(path);

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);

    // 目录本身移除时，其下资源记录一并清掉（这是用户显式操作，不是自动清理）
    q.prepare(QStringLiteral("DELETE FROM media_items WHERE folder_path = ? OR folder_path LIKE ?"));
    q.addBindValue(norm);
    q.addBindValue(norm + QLatin1Char('/') + QLatin1Char('%'));
    q.exec();

    q.prepare(QStringLiteral("DELETE FROM folders WHERE path = ?"));
    q.addBindValue(norm);
    q.exec();

    emit libraryChanged();
}

QList<QString> MediaDatabase::allFolderPaths() const
{
    QList<QString> result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT path FROM folders ORDER BY path"))) {
        while (q.next())
            result.append(q.value(0).toString());
    }
    return result;
}

bool MediaDatabase::folderExists(const QString &path) const
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM folders WHERE path = ? LIMIT 1"));
    q.addBindValue(normalizePath(path));
    return q.exec() && q.next();
}

// ---------------------------------------------------------------- 写入

int MediaDatabase::upsertItems(const QList<MediaItem> &items, qint64 folderId)
{
    Q_UNUSED(folderId)
    if (items.isEmpty())
        return 0;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    if (!db.isOpen())
        return 0;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int affected = 0;

    // 一个事务包住整批；SQLite 的写锁只在事务期间持有，比逐条快一个数量级
    const bool inTrans = db.transaction();

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(R"(
        INSERT INTO media_items
            (path, folder_path, file_name, type, size, mtime,
             content_hash, width, height, duration_ms, camera_make, camera_model, taken_at, indexed_at)
        VALUES
            (:path, :folder, :name, :type, :size, :mtime,
             :chash, :w, :h, :dur, :make, :model, :taken, :indexed)
    )"));

    QSqlQuery upd(db);
    upd.prepare(QStringLiteral(R"(
        UPDATE media_items SET
            file_name = :name, type = :type, size = :size, mtime = :mtime,
            content_hash = NULL, visual_hash = NULL,
            width = 0, height = 0, duration_ms = -1,
            missing = 0, indexed_at = :indexed
        WHERE path = :path
    )"));

    // 只查已存在记录的 mtime/size，用来判断"是否真的变了"
    QSqlQuery exist(db);
    exist.prepare(QStringLiteral("SELECT id, mtime, size FROM media_items WHERE path = ?"));

    QSqlQuery touch(db);
    touch.prepare(QStringLiteral(
        "UPDATE media_items SET missing = 0, indexed_at = :t WHERE id = :id"));

    for (const MediaItem &item : items) {
        const QString norm = normalizePath(item.path);
        if (norm.isEmpty())
            continue;

        exist.bindValue(0, norm);
        if (!exist.exec())
            continue;

        const bool found = exist.next();
        const qint64 existId = found ? exist.value(0).toLongLong() : 0;
        const qint64 existMtime = found ? exist.value(1).toLongLong() : 0;
        const qint64 existSize = found ? exist.value(2).toLongLong() : 0;
        exist.finish();

        const qint64 mtime = item.mtimeMs > 0
                                 ? item.mtimeMs
                                 : item.modified.toMSecsSinceEpoch();

        if (!found) {
            ins.bindValue(QStringLiteral(":path"), norm);
            ins.bindValue(QStringLiteral(":folder"), normalizePath(item.folderPath));
            ins.bindValue(QStringLiteral(":name"), item.fileName.isEmpty()
                                                       ? QFileInfo(norm).fileName()
                                                       : item.fileName);
            ins.bindValue(QStringLiteral(":type"), static_cast<int>(item.type));
            ins.bindValue(QStringLiteral(":size"), item.size);
            ins.bindValue(QStringLiteral(":mtime"), mtime);
            ins.bindValue(QStringLiteral(":chash"), item.contentHash.isEmpty()
                                                         ? QVariant()
                                                         : item.contentHash);
            ins.bindValue(QStringLiteral(":w"), item.width);
            ins.bindValue(QStringLiteral(":h"), item.height);
            ins.bindValue(QStringLiteral(":dur"), item.durationMs);
            ins.bindValue(QStringLiteral(":make"), item.cameraMake);
            ins.bindValue(QStringLiteral(":model"), item.cameraModel);
            ins.bindValue(QStringLiteral(":taken"), item.takenAt.isValid()
                                                        ? item.takenAt.toMSecsSinceEpoch()
                                                        : QVariant());
            ins.bindValue(QStringLiteral(":indexed"), now);

            if (ins.exec())
                ++affected;

        } else if (existMtime != mtime || existSize != item.size) {
            // 内容变了：清掉旧的 hash 与尺寸，让后台重新算，避免拿过期哈希做判重
            upd.bindValue(QStringLiteral(":name"), item.fileName.isEmpty()
                                                       ? QFileInfo(norm).fileName()
                                                       : item.fileName);
            upd.bindValue(QStringLiteral(":type"), static_cast<int>(item.type));
            upd.bindValue(QStringLiteral(":size"), item.size);
            upd.bindValue(QStringLiteral(":mtime"), mtime);
            upd.bindValue(QStringLiteral(":indexed"), now);
            upd.bindValue(QStringLiteral(":path"), norm);

            if (upd.exec())
                ++affected;

        } else {
            // 未变化：只复位 missing 标记。
            // 注意不覆盖 favorite / tags，用户数据必须在重扫时存活。
            touch.bindValue(QStringLiteral(":t"), now);
            touch.bindValue(QStringLiteral(":id"), existId);
            touch.exec();
        }
    }

    if (inTrans && !db.commit()) {
        m_lastError = db.lastError().text();
        db.rollback();
        emit errorOccurred(m_lastError);
        return 0;
    }

    if (affected > 0)
        emit libraryChanged();

    return affected;
}

bool MediaDatabase::updateItem(const MediaItem &item)
{
    if (item.id <= 0)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral(R"(
        UPDATE media_items SET
            favorite = :fav, missing = :miss,
            content_hash = :chash, visual_hash = :vhash,
            width = :w, height = :h, duration_ms = :dur,
            camera_make = :make, camera_model = :model, taken_at = :taken
        WHERE id = :id
    )"));
    q.bindValue(QStringLiteral(":fav"), item.favorite ? 1 : 0);
    q.bindValue(QStringLiteral(":miss"), item.missing ? 1 : 0);
    q.bindValue(QStringLiteral(":chash"), item.contentHash.isEmpty() ? QVariant() : item.contentHash);
    q.bindValue(QStringLiteral(":vhash"), item.visualHash.isEmpty() ? QVariant() : item.visualHash);
    q.bindValue(QStringLiteral(":w"), item.width);
    q.bindValue(QStringLiteral(":h"), item.height);
    q.bindValue(QStringLiteral(":dur"), item.durationMs);
    q.bindValue(QStringLiteral(":make"), item.cameraMake);
    q.bindValue(QStringLiteral(":model"), item.cameraModel);
    q.bindValue(QStringLiteral(":taken"), item.takenAt.isValid() ? item.takenAt.toMSecsSinceEpoch() : QVariant());
    q.bindValue(QStringLiteral(":id"), item.id);

    const bool ok = q.exec();
    if (!ok)
        m_lastError = q.lastError().text();
    return ok;
}

bool MediaDatabase::setFavorite(qint64 itemId, bool favorite)
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE media_items SET favorite = ? WHERE id = ?"));
    q.addBindValue(favorite ? 1 : 0);
    q.addBindValue(itemId);

    const bool ok = q.exec();
    if (ok)
        emit libraryChanged();
    return ok;
}

int MediaDatabase::markMissing(const QString &folderPath, const QStringList &seenPaths)
{
    const QString norm = normalizePath(folderPath);

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    if (!db.transaction())
        return 0;

    // 用临时表装"本次见到了哪些"，再一条 UPDATE 把没见到的标失效。
    // 比在 C++ 里拉全部路径出来做差集省内存，也不用拼超长 IN 列表。
    QSqlQuery q(db);
    q.exec(QStringLiteral("CREATE TEMP TABLE IF NOT EXISTS _seen_paths(path TEXT PRIMARY KEY)"));
    q.exec(QStringLiteral("DELETE FROM _seen_paths"));

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT OR IGNORE INTO _seen_paths(path) VALUES(?)"));

    for (const QString &p : seenPaths) {
        ins.bindValue(0, normalizePath(p));
        ins.exec();
    }

    QSqlQuery upd(db);
    upd.prepare(QStringLiteral(R"(
        UPDATE media_items
        SET missing = 1
        WHERE (folder_path = :folder OR folder_path LIKE :prefix)
          AND missing = 0
          AND path NOT IN (SELECT path FROM _seen_paths)
    )"));
    upd.bindValue(QStringLiteral(":folder"), norm);
    upd.bindValue(QStringLiteral(":prefix"), norm + QLatin1Char('/') + QLatin1Char('%'));
    upd.exec();

    const int affected = upd.numRowsAffected();

    db.commit();

    if (affected > 0)
        emit libraryChanged();

    return affected;
}

int MediaDatabase::purgeMissing()
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM media_items WHERE missing = 1"));
    const int n = q.numRowsAffected();
    if (n > 0)
        emit libraryChanged();
    return n;
}

bool MediaDatabase::markMissingByPath(const QString &path)
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE media_items SET missing = 1 WHERE path = ?"));
    q.addBindValue(normalizePath(path));
    const bool ok = q.exec();
    if (ok && q.numRowsAffected() > 0)
        emit libraryChanged();
    return ok;
}

bool MediaDatabase::relocatePath(const QString &oldPath, const QString &newPath)
{
    const QString oldNorm = normalizePath(oldPath);
    const QString newNorm = normalizePath(newPath);

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    QSqlQuery check(db);
    check.prepare(QStringLiteral("SELECT 1 FROM media_items WHERE path = ?"));
    check.addBindValue(newNorm);
    if (check.exec() && check.next())
        return false;   // 新路径已存在记录，交由 upsert 处理，避免 UNIQUE 冲突

    QSqlQuery q(db);
    q.prepare(QStringLiteral(R"(
        UPDATE media_items
        SET path = :new, file_name = :name, folder_path = :folder, missing = 0
        WHERE path = :old
    )"));
    q.bindValue(QStringLiteral(":new"), newNorm);
    q.bindValue(QStringLiteral(":name"), QFileInfo(newNorm).fileName());
    q.bindValue(QStringLiteral(":folder"), QFileInfo(newNorm).absolutePath());
    q.bindValue(QStringLiteral(":old"), oldNorm);

    const bool ok = q.exec();
    if (ok && q.numRowsAffected() > 0)
        emit libraryChanged();
    return ok;
}

bool MediaDatabase::renameItem(const QString &oldPath, const QString &newPath)
{
    const QString oldNorm = normalizePath(oldPath);
    const QString newNorm = normalizePath(newPath);

    if (oldNorm.isEmpty() || newNorm.isEmpty() || oldNorm == newNorm)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    // 先查目标路径是否已被占用，给出可区分的失败原因
    QSqlQuery check(db);
    check.prepare(QStringLiteral("SELECT 1 FROM media_items WHERE path = ?"));
    check.addBindValue(newNorm);
    if (check.exec() && check.next()) {
        m_lastError = QStringLiteral("目标名称已被其他记录占用");
        return false;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(R"(
        UPDATE media_items
        SET path = :new, file_name = :name, folder_path = :folder
        WHERE path = :old
    )"));
    q.bindValue(QStringLiteral(":new"), newNorm);
    q.bindValue(QStringLiteral(":name"), QFileInfo(newNorm).fileName());
    q.bindValue(QStringLiteral(":folder"), QFileInfo(newNorm).absolutePath());
    q.bindValue(QStringLiteral(":old"), oldNorm);

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }

    if (q.numRowsAffected() > 0)
        emit libraryChanged();

    return true;
}

// ---------------------------------------------------------------- 查询

namespace {

/*! 把一行 SELECT 结果映射成 MediaItem。列顺序必须与 itemSelectColumns() 一致。 */
MediaItem itemFromQuery(const QSqlQuery &q)
{
    MediaItem it;
    it.id           = q.value(0).toLongLong();
    it.path         = q.value(1).toString();
    it.folderPath   = q.value(2).toString();
    it.fileName     = q.value(3).toString();
    it.type         = typeFromInt(q.value(4).toInt());
    it.size         = q.value(5).toLongLong();
    it.mtimeMs      = q.value(6).toLongLong();
    it.modified     = QDateTime::fromMSecsSinceEpoch(it.mtimeMs);
    it.favorite     = q.value(7).toInt() != 0;
    it.missing      = q.value(8).toInt() != 0;
    it.contentHash  = q.value(9).toString();
    it.visualHash   = q.value(10).toString();
    it.width        = q.value(11).toInt();
    it.height       = q.value(12).toInt();
    it.durationMs   = q.value(13).toDouble();
    it.cameraMake   = q.value(14).toString();
    it.cameraModel  = q.value(15).toString();

    const QVariant taken = q.value(16);
    it.takenAt = taken.isNull() ? QDateTime() : QDateTime::fromMSecsSinceEpoch(taken.toLongLong());
    if (!it.takenAt.isValid())
        it.takenAt = it.modified;

    return it;
}

const char *kItemColumns =
    "id, path, folder_path, file_name, type, size, mtime, favorite, missing, "
    "content_hash, visual_hash, width, height, duration_ms, camera_make, camera_model, taken_at";

/*!
 * 转义 LIKE 模式里的通配符。
 *
 * 不转义的话，用户搜 "50%" 会被 SQL 当成"以 50 开头的任意串"，
 * 搜 "_" 会匹配任意单字符 —— 搜出来的结果和用户看到的输入不是一回事。
 * 配合 SQL 里的 ESCAPE '\' 使用。
 */
QString escapeLikePattern(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return escaped;
}

} // namespace

MediaItem MediaDatabase::itemById(qint64 id) const
{
    if (id <= 0)
        return MediaItem();

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    // 按主键查，只可能一行；不加 missing 过滤——调用方可能正需要看失效条目
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM media_items WHERE id = ?")
                  .arg(QString::fromLatin1(kItemColumns)));
    q.bindValue(0, id);

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qWarning() << "[MediaDatabase] itemById failed:" << m_lastError;
        return MediaItem();
    }

    if (!q.next())
        return MediaItem();

    return itemFromQuery(q);
}

QList<MediaItem> MediaDatabase::queryItems(const QString &folderPath, MediaType type,
                                           bool onlyFavorites, int offset, int limit,
                                           const QString &orderBy, const QString &searchText,
                                           bool includeMissing) const
{
    QList<MediaItem> result;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QString sql = QStringLiteral("SELECT %1 FROM media_items WHERE 1=1")
                      .arg(QString::fromLatin1(kItemColumns));
    QVariantList binds;

    if (!folderPath.isEmpty()) {
        /*!
         * 目录查询包含子目录：用户点 "Photos" 期望看到里面的所有图。
         *
         * 关于 `OR ... LIKE 'root/%'` 的写法（实测记录，20 万条 / build/_or_test.py）：
         * 这个条件会让 SQLite 放弃为它选目录索引——优化器无法把 OR 的两个分支
         * 分别走不同索引再合并，于是整条目录条件退化为行级过滤：
         *
         *   OR + LIKE 前缀通配符     27.68 ms
         *   folder_path = ? 等值      0.03 ms
         *
         * 曾尝试改成范围比较（folder_path >= ? AND folder_path < ?）让索引生效，
         * 但上界无法正确表达"所有后代路径"：D:/Photos/2019 之类的子目录都大于
         * D:/Photos/ 这个上界，会被全部漏掉。边界测试已复现该失败
         * （build/_range_test.py），因此保持 OR + LIKE，正确性优先。
         *
         * 实际体验中的缓解：常用排序走 idx_items_mtime 做索引扫描，
         * 目录过滤叠加其上，只在"进入深层子目录 + 同时按类型筛选"时才明显变慢。
         */
        const QString norm = normalizePath(folderPath);
        sql += QStringLiteral(" AND (folder_path = ? OR folder_path LIKE ?)");
        binds << norm << (norm + QLatin1Char('/') + QLatin1Char('%'));
    }
    if (type != MediaType::Unknown) {
        sql += QStringLiteral(" AND type = ?");
        binds << static_cast<int>(type);
    }
    if (onlyFavorites)
        sql += QStringLiteral(" AND favorite = 1");

    // 搜索走文件名的子串匹配。
    // LIKE '%x%' 用不上索引，但这是唯一能覆盖全库的写法 ——
    // 只在已加载页里做内存过滤会让用户搜不到没加载到的文件。
    const QString needle = searchText.trimmed();
    if (!needle.isEmpty()) {
        sql += QStringLiteral(" AND file_name LIKE ? ESCAPE '\\'");
        binds << QStringLiteral("%%%1%%").arg(escapeLikePattern(needle));
    }

    // 「失效记录」视图要看的正是 missing = 1 的行，所以这里不能无条件排除。
    sql += includeMissing ? QStringLiteral(" AND missing = 1")
                          : QStringLiteral(" AND missing = 0");

    // orderBy 只接受内部白名单值，此处直接拼接。
    // 外部字符串一律不进入这里，避免注入。
    sql += QStringLiteral(" ORDER BY ") + orderBy;

    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ? OFFSET ?");
        binds << limit << offset;
    }

    QSqlQuery q(db);
    q.prepare(sql);
    for (int i = 0; i < binds.size(); ++i)
        q.bindValue(i, binds.at(i));

    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qWarning() << "[MediaDatabase] queryItems failed:" << m_lastError;
        return result;
    }

    while (q.next())
        result.append(itemFromQuery(q));

    return result;
}

int MediaDatabase::countItems(const QString &folderPath, MediaType type, bool onlyFavorites,
                              const QString &searchText, bool includeMissing) const
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QString sql = includeMissing
                      ? QStringLiteral("SELECT COUNT(*) FROM media_items WHERE missing = 1")
                      : QStringLiteral("SELECT COUNT(*) FROM media_items WHERE missing = 0");
    QVariantList binds;

    if (!folderPath.isEmpty()) {
        // 与 queryItems 的条件必须逐条对应（分页总数与可取行数要一致）。
        // 这里同样用 OR + LIKE，理由与实测记录见 queryItems 中的说明。
        const QString norm = normalizePath(folderPath);
        sql += QStringLiteral(" AND (folder_path = ? OR folder_path LIKE ?)");
        binds << norm << (norm + QLatin1Char('/') + QLatin1Char('%'));
    }
    if (type != MediaType::Unknown) {
        sql += QStringLiteral(" AND type = ?");
        binds << static_cast<int>(type);
    }
    if (onlyFavorites)
        sql += QStringLiteral(" AND favorite = 1");

    // 与 queryItems 的条件必须逐条对应，否则分页的"总数"与实际能取到的行数不一致
    const QString needle = searchText.trimmed();
    if (!needle.isEmpty()) {
        sql += QStringLiteral(" AND file_name LIKE ? ESCAPE '\\'");
        binds << QStringLiteral("%%%1%%").arg(escapeLikePattern(needle));
    }

    QSqlQuery q(db);
    q.prepare(sql);
    for (int i = 0; i < binds.size(); ++i)
        q.bindValue(i, binds.at(i));

    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

QHash<QString, int> MediaDatabase::folderCounts() const
{
    QHash<QString, int> result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);

    // 一次 GROUP BY 拿到所有目录计数，避免对每个目录单独 count（N+1 查询）
    if (q.exec(QStringLiteral(
            "SELECT folder_path, COUNT(*) FROM media_items "
            "WHERE missing = 0 GROUP BY folder_path"))) {
        while (q.next())
            result.insert(q.value(0).toString(), q.value(1).toInt());
    }
    return result;
}

QHash<int, int> MediaDatabase::typeCounts() const
{
    QHash<int, int> result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    if (q.exec(QStringLiteral(
            "SELECT type, COUNT(*) FROM media_items WHERE missing = 0 GROUP BY type"))) {
        while (q.next())
            result.insert(q.value(0).toInt(), q.value(1).toInt());
    }
    return result;
}

QList<MediaItem> MediaDatabase::allItems(bool includeMissing) const
{
    QList<MediaItem> result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    QString sql = QStringLiteral("SELECT %1 FROM media_items").arg(QString::fromLatin1(kItemColumns));
    if (!includeMissing)
        sql += QStringLiteral(" WHERE missing = 0");
    sql += QStringLiteral(" ORDER BY mtime DESC");

    QSqlQuery q(db);
    if (!q.exec(sql))
        return result;
    while (q.next())
        result.append(itemFromQuery(q));
    return result;
}

QList<MediaItem> MediaDatabase::itemsWithoutHash(int limit) const
{
    QList<MediaItem> result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM media_items "
                             "WHERE content_hash IS NULL AND missing = 0 "
                             "LIMIT ?").arg(QString::fromLatin1(kItemColumns)));
    q.addBindValue(limit);
    if (!q.exec())
        return result;
    while (q.next())
        result.append(itemFromQuery(q));
    return result;
}

bool MediaDatabase::setContentHash(qint64 itemId, const QString &hash)
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE media_items SET content_hash = ? WHERE id = ?"));
    q.addBindValue(hash.isEmpty() ? QVariant() : hash);
    q.addBindValue(itemId);
    return q.exec();
}

bool MediaDatabase::setVisualHash(qint64 itemId, const QString &hash)
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE media_items SET visual_hash = ? WHERE id = ?"));
    q.addBindValue(hash.isEmpty() ? QVariant() : hash);
    q.addBindValue(itemId);
    return q.exec();
}

bool MediaDatabase::setDimensions(qint64 itemId, int width, int height, double durationMs)
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE media_items SET width = ?, height = ?, duration_ms = ? WHERE id = ?"));
    q.addBindValue(width);
    q.addBindValue(height);
    q.addBindValue(durationMs);
    q.addBindValue(itemId);
    return q.exec();
}

QList<MediaItem> MediaDatabase::itemsByContentHash(const QString &hash) const
{
    QList<MediaItem> result;
    if (hash.isEmpty())
        return result;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM media_items WHERE content_hash = ?")
                  .arg(QString::fromLatin1(kItemColumns)));
    q.addBindValue(hash);
    if (!q.exec())
        return result;
    while (q.next())
        result.append(itemFromQuery(q));
    return result;
}

QHash<QString, QList<MediaItem>> MediaDatabase::duplicateGroups() const
{
    QHash<QString, QList<MediaItem>> groups;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    // 先用 SQL 找出重复的哈希值，再只把这些哈希对应的行取回来。
    // 避免把整库拉进内存做分组。
    QSqlQuery hq(db);
    if (!hq.exec(QStringLiteral(
            "SELECT content_hash FROM media_items "
            "WHERE content_hash IS NOT NULL AND missing = 0 "
            "GROUP BY content_hash HAVING COUNT(*) > 1"))) {
        return groups;
    }

    QStringList hashes;
    while (hq.next())
        hashes.append(hq.value(0).toString());

    if (hashes.isEmpty())
        return groups;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM media_items WHERE content_hash = ? AND missing = 0")
                  .arg(QString::fromLatin1(kItemColumns)));

    for (const QString &h : hashes) {
        q.bindValue(0, h);
        if (!q.exec())
            continue;
        QList<MediaItem> items;
        while (q.next())
            items.append(itemFromQuery(q));
        if (items.size() > 1)
            groups.insert(h, items);
    }

    return groups;
}

// ---------------------------------------------------------------- 标签

QStringList MediaDatabase::allTags() const
{
    QStringList result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT name FROM tags ORDER BY name"))) {
        while (q.next())
            result.append(q.value(0).toString());
    }
    return result;
}

QStringList MediaDatabase::tagsForItem(qint64 itemId) const
{
    QStringList result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    /*!
     * 按绑定顺序（rowid）而不是标签名排序。
     *
     * 按名字排的问题：中文按 UTF-8 字节序排出来接近随机，用户刚加的标签
     * 会"跳"到列表中间某处——看起来像加错了位置。
     * 按 rowid 排则新标签永远追加在末尾，符合"加一个新的排最后"的直觉；
     * 取消再重新绑定会拿到新的 rowid，同样落在末尾，语义一致。
     * 不改 schema，rowid 是 SQLite 表自带的。
     */
    q.prepare(QStringLiteral(
        "SELECT t.name FROM tags t "
        "JOIN item_tags it ON it.tag_id = t.id "
        "WHERE it.item_id = ? ORDER BY it.rowid"));
    q.addBindValue(itemId);
    if (q.exec()) {
        while (q.next())
            result.append(q.value(0).toString());
    }
    return result;
}

bool MediaDatabase::addTag(qint64 itemId, const QString &tag)
{
    const QString name = tag.trimmed();
    if (name.isEmpty() || itemId <= 0)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    QSqlQuery tq(db);
    tq.prepare(QStringLiteral("INSERT OR IGNORE INTO tags(name) VALUES(?)"));
    tq.addBindValue(name);
    tq.exec();

    QSqlQuery idq(db);
    idq.prepare(QStringLiteral("SELECT id FROM tags WHERE name = ?"));
    idq.addBindValue(name);
    if (!idq.exec() || !idq.next())
        return false;

    QSqlQuery lq(db);
    lq.prepare(QStringLiteral("INSERT OR IGNORE INTO item_tags(item_id, tag_id) VALUES(?, ?)"));
    lq.addBindValue(itemId);
    lq.addBindValue(idq.value(0).toLongLong());
    return lq.exec();
}

bool MediaDatabase::removeTag(qint64 itemId, const QString &tag)
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM item_tags WHERE item_id = ? AND tag_id = (SELECT id FROM tags WHERE name = ?)"));
    q.addBindValue(itemId);
    q.addBindValue(tag.trimmed());
    return q.exec();
}

bool MediaDatabase::deleteTag(const QString &tag)
{
    const QString name = tag.trimmed();
    if (name.isEmpty())
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    // 先解绑、后删标签。反序做的话 tag_id 已不存在，item_tags 里的行删不掉，
    // 会留下永远查不出来的孤儿引用。
    QSqlQuery uq(db);
    uq.prepare(QStringLiteral(
        "DELETE FROM item_tags WHERE tag_id = (SELECT id FROM tags WHERE name = ?)"));
    uq.addBindValue(name);
    if (!uq.exec())
        return false;

    QSqlQuery dq(db);
    dq.prepare(QStringLiteral("DELETE FROM tags WHERE name = ?"));
    dq.addBindValue(name);
    return dq.exec();
}

QHash<qint64, QStringList> MediaDatabase::tagsForItems(const QList<qint64> &itemIds) const
{
    QHash<qint64, QStringList> result;
    if (itemIds.isEmpty())
        return result;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    // 网格滚动时会频繁请求，这里一次取回整批而不是逐项查询
    QStringList placeholders;
    placeholders.reserve(itemIds.size());
    for (int i = 0; i < itemIds.size(); ++i)
        placeholders << QStringLiteral("?");

    QSqlQuery q(db);
    // item_id 保证各条目的标签分组不串，rowid 保证组内顺序与
    // tagsForItem 一致（新绑定的标签排最后）
    q.prepare(QStringLiteral("SELECT it.item_id, t.name FROM item_tags it "
                             "JOIN tags t ON t.id = it.tag_id "
                             "WHERE it.item_id IN (%1) "
                             "ORDER BY it.item_id, it.rowid")
                  .arg(placeholders.join(QLatin1Char(','))));
    for (qint64 id : itemIds)
        q.addBindValue(id);

    if (q.exec()) {
        while (q.next())
            result[q.value(0).toLongLong()].append(q.value(1).toString());
    }
    return result;
}

// ---------------------------------------------------------------- 播放记录

bool MediaDatabase::recordPlayback(qint64 itemId, double positionMs)
{
    if (itemId <= 0)
        return false;

    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    // UPSERT：同一资源只保留一行，重复播放累加计数
    q.prepare(QStringLiteral(R"(
        INSERT INTO play_history(item_id, last_play, position_ms, play_count)
        VALUES(:id, :t, :pos, 1)
        ON CONFLICT(item_id) DO UPDATE SET
            last_play   = :t,
            position_ms = :pos,
            play_count  = play_count + 1
    )"));
    q.bindValue(QStringLiteral(":id"), itemId);
    q.bindValue(QStringLiteral(":t"), QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":pos"), positionMs);

    if (!q.exec())
        return false;

    // 播放记录会改变首页「最近浏览」的内容与顺序、侧边栏「最近」计数，
    // 所以要和 setFavorite / clearPlaybackHistory 一样广播。
    // 侧边栏侧有 600ms 去抖，PreviewDialog 侧有 5 秒节流，不会打爆 UI。
    emit libraryChanged();
    return true;
}

QList<MediaItem> MediaDatabase::recentPlayed(int limit) const
{
    QList<MediaItem> result;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    // 与 queryItems 同口径：排除失效记录，否则文件被删后
    // 「最近浏览」里会留一条点不开的死记录。
    q.prepare(QStringLiteral(
        "SELECT %1 FROM media_items m "
        "JOIN play_history p ON p.item_id = m.id "
        "WHERE m.missing = 0 "
        "ORDER BY p.last_play DESC LIMIT ?").arg(QString::fromLatin1(kItemColumns)));
    q.addBindValue(limit);
    if (!q.exec())
        return result;
    while (q.next())
        result.append(itemFromQuery(q));
    return result;
}

int MediaDatabase::playHistoryCount() const
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    // 只数行，不连表查回 MediaItem。侧边栏只需要一个数字。
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM play_history")) && q.next())
        return q.value(0).toInt();
    return 0;
}

int MediaDatabase::playCount(qint64 itemId) const
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT play_count FROM play_history WHERE item_id = ?"));
    q.addBindValue(itemId);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

double MediaDatabase::lastPosition(qint64 itemId) const
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT position_ms FROM play_history WHERE item_id = ?"));
    q.addBindValue(itemId);
    if (q.exec() && q.next())
        return q.value(0).toDouble();
    return 0.0;
}

void MediaDatabase::clearPlaybackHistory()
{
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM play_history"));
    emit libraryChanged();
}

// ---------------------------------------------------------------- 统计

MediaDatabase::Stats MediaDatabase::stats() const
{
    Stats s;
    QSqlDatabase db = QSqlDatabase::database(connectionForCurrentThread());

    QSqlQuery q(db);
    if (q.exec(QStringLiteral(
            "SELECT type, COUNT(*), COALESCE(SUM(size), 0) FROM media_items "
            "WHERE missing = 0 GROUP BY type"))) {
        while (q.next()) {
            const int type = q.value(0).toInt();
            const int count = q.value(1).toInt();
            s.totalBytes += q.value(2).toLongLong();
            s.total += count;
            if (type == 1) s.images = count;
            else if (type == 2) s.videos = count;
            else if (type == 3) s.audios = count;
        }
    }

    // 收藏数必须排除失效记录，与 queryItems/countItems 的口径保持一致
    // （那两处都强制 missing = 0）。否则首页卡片的数字会比点进去看到的条数多。
    QSqlQuery fq(db);
    if (fq.exec(QStringLiteral(
            "SELECT COUNT(*) FROM media_items WHERE favorite = 1 AND missing = 0"))
        && fq.next()) {
        s.favorites = fq.value(0).toInt();
    }

    QSqlQuery mq(db);
    if (mq.exec(QStringLiteral("SELECT COUNT(*) FROM media_items WHERE missing = 1"))
        && mq.next()) {
        s.missing = mq.value(0).toInt();
    }

    return s;
}

} // namespace media
