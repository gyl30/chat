#include "theme_manager.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QWidget>

namespace
{
struct theme_data
{
    QHash<QString, QColor> colors;
    QList<QColor> avatars;
    QString sheet;
    std::unique_ptr<QTemporaryDir> icons;
};

constexpr char const* required_roles[]{
    "canvas", "canvas_hover", "panel", "surface", "subtle", "selected", "bubble_in", "bubble_out",
    "reaction_own", "border", "border_strong", "focus", "accent_muted", "text", "text_secondary",
    "text_muted", "accent", "accent_hover", "accent_pressed", "accent_disabled", "online", "link",
    "danger", "danger_hover", "danger_soft", "nav", "nav_text", "on_accent", "avatar_text", "highlight",
    "badge_text", "message_sender", "reaction_text", "day_pill", "message_focus", "message_muted",
    "read", "nav_selected", "nav_hover", "nav_selected_text", "input_border", "input_hover"
};

QString appearance_key(chat_appearance value)
{
    if (value == chat_appearance::light) { return QStringLiteral("light"); }
    if (value == chat_appearance::dark) { return QStringLiteral("dark"); }
    return QStringLiteral("system");
}

QString packaged_directory()
{
    QDir executable(QCoreApplication::applicationDirPath());
    auto const build = executable.filePath(QStringLiteral("themes"));
    if (QFile::exists(build + QStringLiteral("/common.qss"))) { return build; }
    return executable.filePath(QStringLiteral(CHAT_QT_INSTALLED_THEMES));
}

bool read_file(QString const& path, QByteArray& contents, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("无法读取主题文件 %1：%2").arg(path, file.errorString());
        return false;
    }
    contents = file.readAll();
    if (file.error() != QFileDevice::NoError)
    {
        error = QStringLiteral("读取主题文件失败 %1：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool expand(QString& text, QHash<QString, QColor> const& colors, QString& error)
{
    QRegularExpression const token(QStringLiteral(R"(\$\{([a-z_]+)(\.rgb)?\})"));
    auto matches = token.globalMatch(text);
    while (matches.hasNext())
    {
        auto const match = matches.next();
        auto const it = colors.constFind(match.captured(1));
        if (it == colors.cend())
        {
            error = QStringLiteral("主题样式引用了未知颜色：%1").arg(match.captured(1));
            return false;
        }
    }
    for (auto it = colors.cbegin(); it != colors.cend(); ++it)
    {
        text.replace(QStringLiteral("${%1.rgb}").arg(it.key()),
                     QStringLiteral("%1, %2, %3").arg(it->red()).arg(it->green()).arg(it->blue()));
        text.replace(QStringLiteral("${%1}").arg(it.key()), it->name(QColor::HexRgb));
    }
    if (text.contains(QStringLiteral("${")))
    {
        error = QStringLiteral("主题样式包含无效的颜色占位符");
        return false;
    }
    return true;
}

bool load(QString const& key, bool overrides, theme_data& data, QString& error)
{
    QDir packaged(packaged_directory());
    QDir user(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/themes"));
    auto path = [&](QString const& name) {
        auto const candidate = user.filePath(name);
        return overrides && QFile::exists(candidate) ? candidate : packaged.filePath(name);
    };
    QByteArray json, common, sheet;
    if (!read_file(path(key + QStringLiteral(".json")), json, error)
        || !read_file(packaged.filePath(QStringLiteral("common.qss")), common, error)
        || !read_file(path(key + QStringLiteral(".qss")), sheet, error)) { return false; }
    QJsonParseError parse_error;
    auto const document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("主题 JSON 无效：%1（%2）").arg(path(key + QStringLiteral(".json")), parse_error.errorString());
        return false;
    }
    auto const object = document.object();
    auto const colors = object.value(QStringLiteral("colors")).toObject();
    auto valid_color = [](QJsonValue const& value) {
        static QRegularExpression const hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
        return value.isString() && hex.match(value.toString()).hasMatch();
    };
    for (auto const* role : required_roles)
    {
        auto const name = QString::fromLatin1(role);
        auto const value = colors.value(name);
        if (!valid_color(value))
        {
            error = QStringLiteral("主题缺少有效的颜色 %1：%2").arg(name, path(key + QStringLiteral(".json")));
            return false;
        }
        data.colors.insert(name, QColor(value.toString()));
    }
    auto const avatars = object.value(QStringLiteral("avatars")).toArray();
    if (avatars.isEmpty()) { error = QStringLiteral("主题缺少头像配色"); return false; }
    for (auto const& value : avatars)
    {
        if (!valid_color(value)) { error = QStringLiteral("主题包含无效的头像颜色"); return false; }
        data.avatars.push_back(QColor(value.toString()));
    }
    data.sheet = QString::fromUtf8(common) + QLatin1Char('\n') + QString::fromUtf8(sheet);
    // QSS cannot pass semantic colors to SVGs. Use unique paths per load so Qt
    // cannot reuse indicators painted for the previous theme.
    data.icons = std::make_unique<QTemporaryDir>();
    if (!data.icons->isValid()) { error = QStringLiteral("无法创建主题图标目录"); return false; }
    for (auto const* name : {"check", "chevron-down"})
    {
        QByteArray icon;
        if (!read_file(QStringLiteral(":/icons/%1.svg").arg(QString::fromLatin1(name)), icon, error)) { return false; }
        auto const role = QString::fromLatin1(name) == QStringLiteral("check") ? "on_accent" : "text_secondary";
        icon.replace("currentColor", data.colors.value(QString::fromLatin1(role)).name().toUtf8());
        auto const icon_path = data.icons->filePath(QStringLiteral("%1.svg").arg(QString::fromLatin1(name)));
        QFile output(icon_path);
        if (!output.open(QIODevice::WriteOnly) || output.write(icon) != icon.size())
        { error = QStringLiteral("无法写入主题图标：%1").arg(icon_path); return false; }
        output.close();
        auto const token = QString::fromLatin1(name) == QStringLiteral("check") ? "${check_icon}" : "${chevron_icon}";
        data.sheet.replace(QString::fromLatin1(token), QDir::fromNativeSeparators(icon_path));
    }
    if (!expand(data.sheet, data.colors, error)) { return false; }
    // Ask Qt itself to parse the complete sheet before changing any live palette.
    // A trailing property proves the parser reached the end; no private CSS API
    // or application-wide message handler is needed.
    QWidget probe;
    probe.setObjectName(QStringLiteral("themeValidationProbe"));
    probe.setStyleSheet(data.sheet + QStringLiteral(
        "\nQWidget#themeValidationProbe { qproperty-windowTitle: \"chat_theme_valid\"; }"));
    probe.ensurePolished();
    if (probe.windowTitle() != QStringLiteral("chat_theme_valid"))
    {
        error = QStringLiteral("主题样式语法无效：%1（共享样式：%2）").arg(
            path(key + QStringLiteral(".qss")), packaged.filePath(QStringLiteral("common.qss")));
        return false;
    }
    return true;
}
} // namespace

theme_manager& theme_manager::instance()
{
    static auto* manager = new theme_manager;
    return *manager;
}

QList<chat_theme_info> const& theme_manager::themes()
{
    static QList<chat_theme_info> const values{
        {chat_theme_id::daylight, QStringLiteral("daylight"), QStringLiteral("Daylight")},
        {chat_theme_id::midnight, QStringLiteral("midnight"), QStringLiteral("Midnight")}
    };
    return values;
}

theme_manager::theme_manager() : QObject(QCoreApplication::instance())
{
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance())) { original_palette_ = application->palette(); }
    auto* hints = QGuiApplication::styleHints();
    system_dark_ = hints->colorScheme() == Qt::ColorScheme::Dark;
    connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme scheme) {
        system_dark_ = scheme == Qt::ColorScheme::Dark;
        if (appearance_ == chat_appearance::system) { reload(); }
    });
    if (QCoreApplication::organizationName().isEmpty()) { appearance_ = chat_appearance::light; }
    reload();
}

void theme_manager::set_theme(chat_theme_id theme)
{
    set_appearance(theme == chat_theme_id::midnight ? chat_appearance::dark : chat_appearance::light);
}

void theme_manager::set_appearance(chat_appearance appearance)
{
    if (appearance_ == appearance) { return; }
    if (apply(appearance)) { save_settings(); }
}

void theme_manager::load_settings()
{
    if (QCoreApplication::organizationName().isEmpty()) { reload(); return; }
    QSettings settings;
    auto const mode = settings.value(QStringLiteral("appearance/mode"), QStringLiteral("system")).toString();
    auto const appearance = mode == QStringLiteral("light") ? chat_appearance::light
        : mode == QStringLiteral("dark") ? chat_appearance::dark : chat_appearance::system;
    if (apply(appearance)) { save_settings(); }
}

void theme_manager::save_settings() const
{
    if (QCoreApplication::organizationName().isEmpty()) { return; }
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/mode"), appearance_key(appearance_));
    // Old light palette names have one explicit successor: Daylight. The saved appearance is retained.
    settings.remove(QStringLiteral("appearance/theme"));
}

bool theme_manager::reload() { return apply(appearance_); }

bool theme_manager::apply(chat_appearance appearance)
{
    auto const night = appearance == chat_appearance::dark || (appearance == chat_appearance::system && system_dark_);
    auto const key = night ? QStringLiteral("midnight") : QStringLiteral("daylight");
    theme_data data;
    QString error;
    bool loaded = load(key, true, data, error);
    if (!loaded)
    {
        data = {};
        QString packaged_error;
        loaded = load(key, false, data, packaged_error);
        if (!loaded) { error += QLatin1Char('\n') + packaged_error; }
    }
    error_ = error;
    if (!error.isEmpty()) { qWarning().noquote() << error; emit load_failed(error); }
    if (!loaded) { return false; }
    auto previous_icons = std::move(icon_directory_);
    icon_directory_ = std::move(data.icons);
    colors_ = std::move(data.colors);
    avatars_ = std::move(data.avatars);
    sheet_ = std::move(data.sheet);
    appearance_ = appearance;
    theme_ = night ? chat_theme_id::midnight : chat_theme_id::daylight;
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance()))
    {
        QPalette palette(original_palette_);
        auto set = [&](QPalette::ColorRole target, char const* source) { palette.setColor(target, color(QString::fromLatin1(source))); };
        set(QPalette::Window, "canvas"); set(QPalette::WindowText, "text");
        set(QPalette::Base, "surface"); set(QPalette::AlternateBase, "subtle");
        set(QPalette::Text, "text"); set(QPalette::Button, "surface"); set(QPalette::ButtonText, "text");
        set(QPalette::Highlight, "accent"); set(QPalette::HighlightedText, "on_accent");
        set(QPalette::ToolTipBase, "surface"); set(QPalette::ToolTipText, "text");
        set(QPalette::PlaceholderText, "text_muted"); set(QPalette::Link, "link");
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        { palette.setColor(QPalette::Disabled, role, color(QStringLiteral("text_muted"))); }
        application->setPalette(palette);
    }
    emit changed();
    return true;
}

QColor theme_manager::color(QString const& role) const
{
    auto const it = colors_.constFind(role);
    Q_ASSERT_X(it != colors_.cend(), "theme_manager::color", qPrintable(role));
    return it != colors_.cend() ? *it : QColor();
}

QColor theme_manager::avatar_background(quint64 seed) const
{
    return avatars_.isEmpty() ? QColor() : avatars_.at(seed % static_cast<quint64>(avatars_.size()));
}

QString theme_manager::style_sheet(QString text) const
{
    QString error;
    if (!expand(text, colors_, error)) { qWarning().noquote() << error; return {}; }
    return text;
}

QColor themed(char const* role) { return theme_manager::instance().color(QString::fromLatin1(role)); }
