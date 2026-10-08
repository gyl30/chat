#include "theme_manager.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleHints>

namespace
{

struct roles
{
    QColor canvas, canvas_hover, panel, surface, subtle, selected, bubble_out, reaction_own;
    QColor border, border_strong, focus, accent_muted;
    QColor text, text_secondary, text_muted;
    QColor accent, accent_hover, accent_disabled, online, link;
    QColor danger, danger_hover, danger_soft;
    QColor nav, nav_text, on_accent, avatar_text;
};

QColor mix(QColor const& a, QColor const& b, double amount)
{
    auto blend = [amount](int x, int y) { return static_cast<int>(x * amount + y * (1.0 - amount) + 0.5); };
    return QColor(blend(a.red(), b.red()), blend(a.green(), b.green()), blend(a.blue(), b.blue()));
}

struct base
{
    char const* canvas;
    char const* canvas_hover;
    char const* panel;
    char const* surface;
    char const* text;
    char const* text_secondary;
    char const* text_muted;
    char const* accent;
    char const* accent_hover;
    char const* link;
    char const* nav;
    char const* nav_text;
};

roles light(base const& value)
{
    roles r;
    r.canvas = QColor(value.canvas);
    r.canvas_hover = QColor(value.canvas_hover);
    r.panel = QColor(value.panel);
    r.surface = QColor(value.surface);
    r.text = QColor(value.text);
    r.text_secondary = QColor(value.text_secondary);
    r.text_muted = QColor(value.text_muted);
    r.accent = QColor(value.accent);
    r.accent_hover = QColor(value.accent_hover);
    r.link = QColor(value.link);
    r.nav = QColor(value.nav);
    r.nav_text = QColor(value.nav_text);
    r.subtle = r.canvas_hover;
    r.selected = mix(r.accent, r.canvas_hover, 0.12);
    r.bubble_out = mix(r.accent, r.surface, 0.14);
    r.reaction_own = mix(r.accent, r.surface, 0.32);
    r.border = mix(r.text, r.canvas, 0.13);
    r.border_strong = mix(r.text, r.canvas, 0.30);
    r.focus = mix(r.accent, r.surface, 0.55);
    r.accent_muted = mix(r.accent, r.text_secondary, 0.6);
    r.accent_disabled = mix(r.accent, r.canvas, 0.35);
    r.online = QColor(QStringLiteral("#2F7D55"));
    r.danger = QColor(QStringLiteral("#A64C48"));
    r.danger_hover = QColor(QStringLiteral("#913F3B"));
    r.danger_soft = QColor(QStringLiteral("#DDAEAA"));
    r.on_accent = QColor(Qt::white);
    r.avatar_text = r.accent;
    return r;
}

// Palettes follow Oink (github.com/pgsty/oink, assets/scss/td/_presets.scss, _brand.scss and
// _experimental-presets.scss); its link color serves as our accent unless the style says otherwise.
roles theme_roles(chat_theme_id theme)
{
    switch (theme)
    {
        case chat_theme_id::paper:
            return light({"#F7F6F3", "#EFEDE8", "#FBFAF8", "#FFFFFF", "#21201C", "#56534C", "#6B665D",
                          "#2B5F8C", "#1D68A5", "#2B5F8C", "#21201C", "#ECE9E3"});
        case chat_theme_id::slate:
            return light({"#F1F4F8", "#E9EEF4", "#F6F9FC", "#FFFFFF", "#16222E", "#3D4E61", "#586B80",
                          "#245F94", "#1D6FC4", "#245F94", "#16222E", "#E8EEF6"});
        case chat_theme_id::ink:
        {
            // Black and white with red accents: actions are black, hover and links turn red.
            // Ink's secondary ground carries the chat, so white message cards stay distinct.
            auto r = light({"#F4F4F4", "#EAEAEA", "#FFFFFF", "#FFFFFF", "#141414", "#474747", "#636363",
                            "#141414", "#C8102E", "#C8102E", "#141414", "#EDEDED"});
            r.selected = QColor(QStringLiteral("#F4F4F4"));
            r.focus = QColor(QStringLiteral("#C8102E"));
            r.avatar_text = r.text;
            return r;
        }
        case chat_theme_id::terminal:
        {
            auto r = light({"#F4F5F2", "#E9EBE6", "#F8F9F6", "#FDFDFB", "#1D211F", "#4A514D", "#606B63",
                            "#0A6560", "#084F4B", "#0A6560", "#1D211F", "#D3DBD6"});
            // Amber marks attention, as Terminal does for its accents.
            r.online = QColor(QStringLiteral("#935400"));
            return r;
        }
        case chat_theme_id::classic:
            break;
    }
    return {};
}

// The night theme, based on Oink Paper's dark palette with the classic green as its action color.
roles night_roles()
{
    roles r;
    r.canvas = QColor(QStringLiteral("#161513"));
    r.canvas_hover = QColor(QStringLiteral("#262420"));
    r.panel = QColor(QStringLiteral("#1B1A17"));
    r.surface = QColor(QStringLiteral("#1F1E1A"));
    r.subtle = QColor(QStringLiteral("#2A2824"));
    r.text = QColor(QStringLiteral("#ECE9E3"));
    r.text_secondary = QColor(QStringLiteral("#B6B1A7"));
    r.text_muted = QColor(QStringLiteral("#958F84"));
    r.accent = QColor(QStringLiteral("#3D7A62"));
    r.accent_hover = QColor(QStringLiteral("#4A8A70"));
    r.accent_disabled = QColor(QStringLiteral("#34413B"));
    r.link = QColor(QStringLiteral("#7DB5E6"));
    r.online = QColor(QStringLiteral("#6FBF95"));
    r.selected = mix(r.accent, r.surface, 0.30);
    r.bubble_out = mix(r.accent, r.surface, 0.42);
    r.reaction_own = mix(r.accent, r.surface, 0.65);
    r.border = QColor(QStringLiteral("#34322D"));
    r.border_strong = QColor(QStringLiteral("#4A4740"));
    r.focus = mix(r.accent, r.text, 0.6);
    r.accent_muted = QColor(QStringLiteral("#8FC1A8"));
    r.danger = QColor(QStringLiteral("#E0716B"));
    r.danger_hover = QColor(QStringLiteral("#C75F59"));
    r.danger_soft = QColor(QStringLiteral("#5A2F2C"));
    r.nav = QColor(QStringLiteral("#0F0E0D"));
    r.nav_text = r.text;
    r.on_accent = QColor(Qt::white);
    r.avatar_text = r.text;
    return r;
}

// Each classic color, by the role it plays in the stylesheet and painters.
QHash<QRgb, QColor> role_map(roles const& r)
{
    QHash<QRgb, QColor> map;
    auto assign = [&map](std::initializer_list<char const*> classic, QColor const& value) {
        for (auto const* hex : classic) { map.insert(QColor(hex).rgb(), value); }
    };
    assign({"#F7F5EF"}, r.canvas);
    assign({"#EEEDE7", "#ECEBE6", "#ECEBE5", "#F1F0ED", "#F0EFEC", "#F1F2EF", "#E8E4DB"}, r.canvas_hover);
    assign({"#FCFBF7", "#FFFEFA"}, r.panel);
    assign({"#FFFFFF"}, r.surface);
    assign({"#F0F4F1", "#EEF1ED", "#ECEFEA", "#E9EEE9", "#E2E8E3", "#F1F3EF", "#EDF1EE"}, r.subtle);
    assign({"#E7EEE9", "#E7EFEA", "#E8F0EB", "#DCEBE3"}, r.selected);
    assign({"#D6EAD9"}, r.bubble_out);
    assign({"#A8D6BD"}, r.reaction_own);
    assign({"#D7DDD9", "#DDD9D0", "#D6D2C8", "#E8E4DA", "#E3E0D8", "#D8D6D0"}, r.border);
    assign({"#AFC0B8", "#A9B8B1"}, r.border_strong);
    assign({"#88A697", "#789487"}, r.focus);
    assign({"#547C68"}, r.accent_muted);
    assign({"#27332E", "#27362F", "#25332D", "#26342E", "#1F2623", "#294B3E", "#3F4542", "#273629"}, r.text);
    assign({"#5D6C64", "#6E7A74", "#747C78", "#78897F", "#465B52", "#586A61"}, r.text_secondary);
    assign({"#8B918D", "#858D88", "#8C948F", "#89918D", "#96A29C", "#6E8877"}, r.text_muted);
    assign({"#315A4B", "#365E4B"}, r.accent);
    assign({"#294D40", "#234536"}, r.accent_hover);
    assign({"#AEBDB6"}, r.accent_disabled);
    assign({"#4F8A70", "#4C876C"}, r.online);
    assign({"#277399"}, r.link);
    assign({"#A64C48", "#D9534F"}, r.danger);
    assign({"#913F3B"}, r.danger_hover);
    assign({"#DDAEAA"}, r.danger_soft);
    assign({"#294F40"}, r.nav);
    assign({"#D8E4DE", "#F4F7F5", "#C4D2CB"}, r.nav_text);
    // Translucent overlays keep their alpha over the mapped base color.
    map.insert(qRgb(39, 54, 47), r.text);
    map.insert(qRgb(70, 91, 82), r.text_secondary);
    map.insert(qRgb(88, 106, 97), r.text_secondary);
    map.insert(qRgb(216, 228, 222), r.nav_text);
    return map;
}

QString appearance_key(chat_appearance value)
{
    switch (value)
    {
        case chat_appearance::light: return QStringLiteral("light");
        case chat_appearance::dark: return QStringLiteral("dark");
        case chat_appearance::system: break;
    }
    return QStringLiteral("system");
}

bool persistent() { return !QCoreApplication::organizationName().isEmpty(); }

}    // namespace

theme_manager& theme_manager::instance()
{
    static auto* manager = new theme_manager;
    return *manager;
}

QList<chat_theme_info> const& theme_manager::themes()
{
    static QList<chat_theme_info> const values{
        {chat_theme_id::classic, QStringLiteral("classic"), QStringLiteral("经典")},
        {chat_theme_id::paper, QStringLiteral("paper"), QStringLiteral("纸张 Paper")},
        {chat_theme_id::slate, QStringLiteral("slate"), QStringLiteral("石板 Slate")},
        {chat_theme_id::ink, QStringLiteral("ink"), QStringLiteral("墨水 Ink")},
        {chat_theme_id::terminal, QStringLiteral("terminal"), QStringLiteral("终端 Terminal")},
    };
    return values;
}

theme_manager::theme_manager()
{
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance()))
    {
        original_palette_ = application->palette();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    auto* hints = QGuiApplication::styleHints();
    system_dark_ = hints->colorScheme() == Qt::ColorScheme::Dark;
    connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme scheme) {
        system_dark_ = scheme == Qt::ColorScheme::Dark;
        if (appearance_ == chat_appearance::system) { rebuild(); }
    });
#endif
    // Tests and tools run without a settings identity; they always start from the classic light theme.
    if (!persistent()) { appearance_ = chat_appearance::light; }
    rebuild();
}

bool theme_manager::dark() const
{
    return appearance_ == chat_appearance::dark || (appearance_ == chat_appearance::system && system_dark_);
}

void theme_manager::set_theme(chat_theme_id theme)
{
    if (theme_ == theme) { return; }
    theme_ = theme;
    save_settings();
    rebuild();
}

void theme_manager::set_appearance(chat_appearance appearance)
{
    if (appearance_ == appearance) { return; }
    appearance_ = appearance;
    save_settings();
    rebuild();
}

void theme_manager::load_settings()
{
    if (!persistent()) { return; }
    QSettings settings;
    auto const theme = settings.value(QStringLiteral("appearance/theme")).toString();
    for (auto const& info : themes())
    {
        if (info.key == theme) { theme_ = info.id; }
    }
    auto const mode = settings.value(QStringLiteral("appearance/mode")).toString();
    appearance_ = mode == QStringLiteral("light") ? chat_appearance::light
        : mode == QStringLiteral("dark") ? chat_appearance::dark : chat_appearance::system;
    rebuild();
}

void theme_manager::save_settings() const
{
    if (!persistent()) { return; }
    QSettings settings;
    for (auto const& info : themes())
    {
        if (info.id == theme_) { settings.setValue(QStringLiteral("appearance/theme"), info.key); }
    }
    settings.setValue(QStringLiteral("appearance/mode"), appearance_key(appearance_));
}

void theme_manager::rebuild()
{
    auto const night = dark();
    identity_ = !night && theme_ == chat_theme_id::classic;
    colors_.clear();
    on_accent_ = QColor(Qt::white);
    avatar_text_ = QColor(QStringLiteral("#315A4B"));
    avatar_ground_ = {};
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!identity_)
    {
        auto const r = night ? night_roles() : theme_roles(theme_);
        colors_ = role_map(r);
        on_accent_ = r.on_accent;
        avatar_text_ = r.avatar_text;
        if (night) { avatar_ground_ = r.surface; }
        if (application)
        {
            // Native parts the stylesheet does not cover (scrollbars, message boxes, tooltips).
            QPalette palette(original_palette_);
            palette.setColor(QPalette::Window, r.canvas);
            palette.setColor(QPalette::WindowText, r.text);
            palette.setColor(QPalette::Base, r.surface);
            palette.setColor(QPalette::AlternateBase, r.subtle);
            palette.setColor(QPalette::Text, r.text);
            palette.setColor(QPalette::Button, r.surface);
            palette.setColor(QPalette::ButtonText, r.text);
            palette.setColor(QPalette::Highlight, r.accent);
            palette.setColor(QPalette::HighlightedText, r.on_accent);
            palette.setColor(QPalette::ToolTipBase, r.surface);
            palette.setColor(QPalette::ToolTipText, r.text);
            palette.setColor(QPalette::PlaceholderText, r.text_muted);
            palette.setColor(QPalette::Link, r.link);
            application->setPalette(palette);
        }
    }
    else if (application)
    {
        application->setPalette(original_palette_);
    }
    emit changed();
}

QColor theme_manager::color(QColor const& classic) const
{
    if (identity_) { return classic; }
    auto const found = colors_.constFind(classic.rgb());
    if (found == colors_.cend()) { return classic; }
    auto value = *found;
    value.setAlpha(classic.alpha());
    return value;
}

QColor theme_manager::on_accent() const { return on_accent_; }

QColor theme_manager::avatar_text() const { return avatar_text_; }

QColor theme_manager::avatar_background(QColor const& classic) const
{
    // Light themes keep the pastel identity colors; night mode tones them down against its surface.
    return avatar_ground_.isValid() ? mix(classic, avatar_ground_, 0.22) : classic;
}

QString theme_manager::style_sheet(QString classic) const
{
    if (identity_) { return classic; }
    // White text sits on accent and navigation grounds; white backgrounds are surfaces.
    static QRegularExpression const on_accent_text(QStringLiteral("((?:^|[\\s;{])(?:selection-)?color:\\s*)#FFFFFF"),
                                                   QRegularExpression::CaseInsensitiveOption);
    classic.replace(on_accent_text, QStringLiteral("\\1@on-accent"));
    static QRegularExpression const hex(QStringLiteral("#[0-9A-Fa-f]{6}\\b"));
    QString result;
    result.reserve(classic.size());
    qsizetype last = 0;
    for (auto it = hex.globalMatch(classic); it.hasNext();)
    {
        auto const match = it.next();
        result += QStringView(classic).mid(last, match.capturedStart() - last);
        result += color(QColor(match.captured())).name(QColor::HexRgb).toUpper();
        last = match.capturedEnd();
    }
    result += QStringView(classic).mid(last);
    static QRegularExpression const rgba(QStringLiteral("rgba\\((\\d+),\\s*(\\d+),\\s*(\\d+),\\s*([0-9.]+)\\)"));
    QString translucent;
    last = 0;
    for (auto it = rgba.globalMatch(result); it.hasNext();)
    {
        auto const match = it.next();
        translucent += QStringView(result).mid(last, match.capturedStart() - last);
        QColor const base(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
        auto const mapped = base == QColor(Qt::white) ? base : color(base);
        translucent += QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(mapped.red()).arg(mapped.green()).arg(mapped.blue()).arg(match.captured(4));
        last = match.capturedEnd();
    }
    translucent += QStringView(result).mid(last);
    translucent.replace(QStringLiteral("@on-accent"), on_accent_.name(QColor::HexRgb).toUpper());
    if (theme_ == chat_theme_id::terminal && !dark())
    {
        // Terminal sets headings and controls in monospace and keeps prose in the sans face.
        translucent += QStringLiteral(R"(
        QPushButton, QToolButton, QLabel#sectionTitle, QLabel#brandLabel, QLabel#windowTitle,
        QPushButton#chatHeaderButton, QLabel#contactCardName {
            font-family: "JetBrains Mono", "DejaVu Sans Mono", "Noto Sans Mono", monospace;
        }
)");
    }
    return translucent;
}

QColor themed(QColor const& classic) { return theme_manager::instance().color(classic); }

QColor themed(char const* classic) { return themed(QColor(classic)); }
