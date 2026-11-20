#pragma once

#include <QDialog>
#include <QImage>
#include <QVariantMap>

class QLabel;
class QPushButton;
class screenshot_preview;

class screenshot_dialog final : public QDialog
{
    Q_OBJECT

   public:
    explicit screenshot_dialog(QWidget* parent);
    ~screenshot_dialog() override;
    QImage selected_image() const;

   private slots:
    void portal_response(uint code, QVariantMap values);

   private:
    void show_image(QImage image);
    void request_screenshot();

    screenshot_preview* preview_;
    QLabel* status_;
    QPushButton* send_;
    QString request_path_;
};
