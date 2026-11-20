#include "clipboard_image.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>

namespace
{
QString image_path(qint64 client_id)
{
    auto const root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return root.isEmpty() ? QString{} : QDir(root).filePath(QStringLiteral("clipboard/图片-%1.png").arg(client_id));
}
}    // namespace

std::expected<QString, QString> save_clipboard_image(QImage const& image, qint64 client_id)
{
    auto path = image_path(client_id);
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        return std::unexpected(QStringLiteral("无法创建剪贴板图片目录"));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    {
        return std::unexpected(file.errorString());
    }
    if (!image.save(&file, "PNG") || !file.flush())
    {
        file.remove();
        return std::unexpected(QStringLiteral("剪贴板图片保存失败，请检查磁盘空间和写入权限"));
    }
    return path;
}

void remove_clipboard_image(QString const& path, qint64 client_id)
{
    if (path == image_path(client_id) && !QFile::remove(path))
    {
        qWarning("无法清理已结束任务的剪贴板图片");
    }
}
