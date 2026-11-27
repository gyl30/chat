#ifndef CHAT_QT_SRC_THEME_HPP
#define CHAT_QT_SRC_THEME_HPP

#include <QLabel>
#include <QSize>
#include <QString>

class QWidget;
class QDialog;

class feedback_label final : public QLabel
{
    Q_OBJECT
   public:
    explicit feedback_label(QWidget* parent = nullptr);
    void show_error(QString message);

   private:
    QObject notification_;
};

namespace chat_theme
{

inline constexpr int window_resize_border = 5;
inline constexpr QSize login_window_size{380, 540};
inline constexpr int auth_card_width = login_window_size.width() - 2 * window_resize_border;
inline constexpr int auth_padding = 36;
inline constexpr int auth_spacing = 12;
inline constexpr int login_avatar_size = 88;

inline constexpr int dialog_padding = 24;
inline constexpr int dialog_spacing = 12;
inline constexpr int dialog_small_width = 480;
inline constexpr int dialog_normal_width = 520;

inline constexpr int dialog_row_height = 66;
inline constexpr int dialog_avatar_size = 46;
inline constexpr int dialog_left = 15;
inline constexpr int dialog_avatar_top = (dialog_row_height - dialog_avatar_size) / 2;
inline constexpr int dialog_text_left = 72;
inline constexpr int dialog_name_top = 12;
inline constexpr int dialog_preview_top = 36;
inline constexpr int dialog_right = 15;
inline constexpr int dialog_unread_height = 19;

inline constexpr int message_max_width = 544;
inline constexpr int message_avatar_size = 45;
inline constexpr int message_avatar_skip = 53;
inline constexpr int message_side_margin = 24;
inline constexpr int message_padding_horizontal = 14;
inline constexpr int message_padding_vertical = 10;
inline constexpr int message_margin_top = 11;
inline constexpr int message_margin_top_attached = 0;
inline constexpr int message_margin_bottom = 3;
inline constexpr int message_radius = 20;
inline constexpr int message_attached_radius = 7;
inline constexpr int message_tail_width = 6;
inline constexpr int message_tail_height = 9;
inline constexpr int message_time_gap = 8;
inline constexpr int message_name_gap = 3;
inline constexpr int message_date_height = 24;
inline constexpr int message_date_margin_top = 14;
inline constexpr int message_date_margin_bottom = 8;

inline constexpr int compose_single_row_min_width = 760;
inline constexpr int compose_height = 60;
inline constexpr int compose_button_width = 40;
inline constexpr int compose_button_height = 40;
inline constexpr int compose_field_min_height = 44;

}    // namespace chat_theme

QString chat_style_sheet();
QLabel* dialog_title(QDialog& dialog, QString title);
void frame_dialog(QDialog& dialog, bool resizable = true);
QString prompt_text(QWidget* parent, QString const& title, QString const& label, QString const& initial, bool& accepted, bool multiline = false);
bool confirm_action(QWidget* parent, QString const& title, QString const& text, QString const& action);

#endif
