#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QSerialPort>
#include <QByteArray>
#include <QJsonObject>

namespace Ui {
class Widget;
}

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = 0);
    ~Widget();

private slots:
    void refreshPorts();
    void openOrCloseSerial();
    void onSerialReadyRead();

    void writeConfigToEsp32();
    void readConfigFromEsp32();
    void clearConfigOnEsp32();

    void chooseQrCodeImage();
    void chooseCoverImage();

private:
    void sendJson(const QJsonObject &obj);
    void handleJsonLine(const QByteArray &line);
    void appendLog(const QString &text);
    void setConfigButtonsEnabled(bool enabled);

    QByteArray imageToRgb565(const QString &path, int w, int h);
    bool sendImageToEsp32(const QString &name, const QByteArray &rgb565, int w, int h);
    void sendImagesIfSelected();

    bool waitForDeviceOk(const QString &expectMsg,
                         int timeoutMs,
                         const QString &expectName = QString());

    bool waitForChunkOk(const QString &expectName, int seq, int timeoutMs);
    void flushEspLine();


private:
    Ui::Widget *ui;
    QSerialPort *serial;
    QByteArray rxBuffer;
    bool lastJsonOk;
    QString lastJsonMsg;
    QJsonObject lastJsonObj;
    int lastChunkSeq;
};

#endif // WIDGET_H
