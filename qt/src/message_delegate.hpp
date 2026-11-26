#ifndef CHAT_QT_SRC_MESSAGE_DELEGATE_HPP
#define CHAT_QT_SRC_MESSAGE_DELEGATE_HPP

#include <QStyledItemDelegate>
#include <QUrl>

class message_delegate final : public QStyledItemDelegate
{
    Q_OBJECT

   public:
    explicit message_delegate(QObject* parent = nullptr, qint64 const* context_anchor = nullptr);

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index) override;

    static bool on_unread_divider(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint position);
    static QString code_at(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint point);
    static QUrl link_at(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint position);
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, QStyleOptionViewItem const& option, QModelIndex const& index) override;

   signals:
    void avatar_clicked(QModelIndex const& index);
    void read_details_clicked(QModelIndex const& index);
    void reaction_clicked(QModelIndex const& index, QString emoji);
    void vote_clicked(QModelIndex const& index, qint64 choice);
    void content_clicked(QModelIndex const& index);
    void image_clicked(QModelIndex const& index);

   private:
    // A read-only context keeps its anchor outlined independently of keyboard focus.
    qint64 const* context_anchor_;
};

#endif
