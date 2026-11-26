#ifndef CHAT_QT_SRC_MESSAGE_DELEGATE_HPP
#define CHAT_QT_SRC_MESSAGE_DELEGATE_HPP

#include <QStyledItemDelegate>
#include <QUrl>

class avatar_cache;

class message_delegate final : public QStyledItemDelegate
{
    Q_OBJECT

   public:
    explicit message_delegate(QObject* parent = nullptr, qint64 const* context_anchor = nullptr, avatar_cache const* avatars = nullptr);

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index) override;

    bool select_text(QAbstractItemView& view, QMouseEvent& event);
    QString selected_text(QModelIndex const& index, QAbstractItemView const& view) const;
    void clear_text_selection();
    QModelIndex text_index() const { return text_index_; }

    static bool on_separator(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint position);
    static QString code_at(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint point);
    static QUrl link_at(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint position);
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, QStyleOptionViewItem const& option, QModelIndex const& index) override;

   signals:
    void reply_clicked(qint64 id);
    void avatar_clicked(QModelIndex const& index);
    void read_details_clicked(QModelIndex const& index);
    void reaction_clicked(QModelIndex const& index, QString emoji);
    void vote_clicked(QModelIndex const& index, qint64 choice);
    void content_clicked(QModelIndex const& index);
    void image_clicked(QModelIndex const& index);

   private:
    QPersistentModelIndex text_index_;
    QPoint press_point_;
    int anchor_ = 0;
    int cursor_ = 0;
    bool dragging_text_ = false;

    // A read-only context keeps its anchor outlined independently of keyboard focus.
    qint64 const* context_anchor_;
    avatar_cache const* avatars_;
};

#endif
