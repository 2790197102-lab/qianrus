#include "widget.h"
#include "ui_widget.h"

#include <QSerialPortInfo>
#include <QSerialPort>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDateTime>
#include <QMessageBox>
#include <QDebug>
#include <QFileDialog>
#include <QImage>
#include <QThread>
#include <QApplication>
#include <QFileInfo>
#include <QElapsedTimer>

Widget::Widget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Widget),
    serial(new QSerialPort(this))
{
    ui->setupUi(this);
    lastJsonOk = false;
    lastJsonMsg.clear();
    lastChunkSeq = -1;

    // 标题栏
    setWindowFlags(Qt::Window
                 | Qt::WindowTitleHint
                 | Qt::WindowSystemMenuHint
                 | Qt::WindowMinMaxButtonsHint
                 | Qt::WindowCloseButtonHint);

    setWindowTitle("ESP-WATCH上位机v1.1");
    setFixedSize(800, 600);

    // 串口号
    refreshPorts();

    // 波特率
    ui->comboBaud->clear();
    ui->comboBaud->addItems(QStringList()
                            << "9600"
                            << "115200"
                            << "230400"
                            << "460800"
                            << "921600");
    ui->comboBaud->setCurrentText("115200");

    ui->comboPort->setFixedWidth(120);
    ui->comboBaud->setFixedWidth(120);

    // 串口接收
    connect(serial, SIGNAL(readyRead()), this, SLOT(onSerialReadyRead()));

    // 连接 / 断开
    connect(ui->btnConnect, SIGNAL(clicked()), this, SLOT(openOrCloseSerial()));

    /*
     * 如果你的 UI 里没有 btnRefreshPort，可以删掉下面这行。
     * 如果有“刷新串口”按钮，objectName 建议设成 btnRefreshPort。
     */
    connect(ui->btnRefreshPort, SIGNAL(clicked()), this, SLOT(refreshPorts()));

    /*
     * 以下三个按钮需要你在 Designer 里设置对应 objectName：
     * btnWriteConfig / btnReadConfig / btnClearConfig
     */
    connect(ui->btnWriteConfig, SIGNAL(clicked()), this, SLOT(writeConfigToEsp32()));
    connect(ui->btnReadConfig, SIGNAL(clicked()), this, SLOT(readConfigFromEsp32()));
    connect(ui->btnClearConfig, SIGNAL(clicked()), this, SLOT(clearConfigOnEsp32()));

    connect(ui->btnQR_code, SIGNAL(clicked()), this, SLOT(chooseQrCodeImage()));
    connect(ui->btnCover, SIGNAL(clicked()), this, SLOT(chooseCoverImage()));

    setConfigButtonsEnabled(false);
    appendLog("程序启动完成，请选择串口并连接设备。");
}

Widget::~Widget()
{
    if(serial->isOpen()) {
        serial->close();
    }

    delete ui;
}

void Widget::refreshPorts()
{
    QString current = ui->comboPort->currentText();

    ui->comboPort->clear();

    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();

    foreach(const QSerialPortInfo &info, ports) {
        QString text = info.portName();

        if(!info.description().isEmpty()) {
            text += " - " + info.description();
        }

        /*
         * comboBox 显示 description，但真正打开串口时用 UserRole 里的 portName。
         */
        ui->comboPort->addItem(text, info.portName());
    }

    for(int i = 0; i < ui->comboPort->count(); ++i) {
        if(ui->comboPort->itemData(i).toString() == current ||
           ui->comboPort->itemText(i).startsWith(current)) {
            ui->comboPort->setCurrentIndex(i);
            break;
        }
    }

    appendLog(QString("发现 %1 个串口。").arg(ports.count()));
}

void Widget::openOrCloseSerial()
{
    if(serial->isOpen()) {
        serial->close();

        ui->btnConnect->setText("连接设备");
        ui->comboPort->setEnabled(true);
        ui->comboBaud->setEnabled(true);
        setConfigButtonsEnabled(false);

        appendLog("串口已关闭。");
        return;
    }

    if(ui->comboPort->count() <= 0) {
        QMessageBox::warning(this, "提示", "没有可用串口，请先连接 ESP32-S3。");
        return;
    }

    QString portName = ui->comboPort->currentData().toString();

    if(portName.isEmpty()) {
        portName = ui->comboPort->currentText();
    }

    int baudRate = ui->comboBaud->currentText().toInt();

    serial->setPortName(portName);
    serial->setBaudRate(baudRate);
    serial->setDataBits(QSerialPort::Data8);
    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setFlowControl(QSerialPort::NoFlowControl);

    rxBuffer.clear();

    if(!serial->open(QIODevice::ReadWrite)) {
        QMessageBox::critical(this,
                              "串口打开失败",
                              serial->errorString());
        appendLog("串口打开失败：" + serial->errorString());
        return;
    }

    ui->btnConnect->setText("断开设备");
    ui->comboPort->setEnabled(false);
    ui->comboBaud->setEnabled(false);
    setConfigButtonsEnabled(true);

    appendLog(QString("串口已打开：%1，波特率：%2")
              .arg(portName)
              .arg(baudRate));
}

void Widget::onSerialReadyRead()
{
    rxBuffer.append(serial->readAll());

    /*
     * ESP32 端是 printf(... "\n")，所以 Qt 端也按 \n 分帧。
     */
    while(rxBuffer.contains('\n')) {
        int index = rxBuffer.indexOf('\n');

        QByteArray line = rxBuffer.left(index).trimmed();
        rxBuffer.remove(0, index + 1);

        if(line.isEmpty()) {
            continue;
        }

        appendLog("RX: " + QString::fromUtf8(line));

        /*
         * ESP-IDF 可能会输出普通日志，比如 I (...) xxx。
         * 只有以 { 开头的才按 JSON 解析。
         */
        int jsonStart = line.indexOf('{');
        if(jsonStart >= 0) {
            handleJsonLine(line.mid(jsonStart));
        }
    }
}

void Widget::sendJson(const QJsonObject &obj)
{
    if(!serial->isOpen()) {
        QMessageBox::warning(this, "提示", "请先连接串口。");
        return;
    }

    QJsonDocument doc(obj);
    QByteArray data = doc.toJson(QJsonDocument::Compact);
    data.append("\n");

    serial->clearError();

    qint64 totalWritten = 0;
    const qint64 totalSize = data.size();

    /*
     * 原来每 32 字节 waitForBytesWritten(1000) 一次，在 Windows + USB Serial/JTAG
     * 上容易因为设备重启、COM 口短暂不可用或驱动缓存未及时刷新而误报超时。
     * 这里改为把一整行 JSON 写入 Qt/系统发送缓存，再统一等待一次。
     */
    while(totalWritten < totalSize) {
        qint64 written = serial->write(data.constData() + totalWritten,
                                       totalSize - totalWritten);
        if(written < 0) {
            appendLog("发送失败：" + serial->errorString());
            QMessageBox::critical(this, "发送失败", serial->errorString());
            return;
        }

        if(written == 0) {
            if(!serial->waitForBytesWritten(3000)) {
                appendLog("发送超时：" + serial->errorString()
                          + "；请确认 VSCode/串口监视器已关闭，并重新连接设备。");
                serial->clear(QSerialPort::Output);
                return;
            }
            continue;
        }

        totalWritten += written;
    }

    if(!serial->waitForBytesWritten(5000)) {
        appendLog("发送超时：" + serial->errorString()
                  + "；如果刚执行过清空/重启，请断开后等待设备重新枚举，再重新连接。");
        serial->clear(QSerialPort::Output);
        return;
    }

    serial->flush();

    // 只用于日志显示，不影响真正发送的数据
    QJsonObject logObj = obj;

    if(logObj.contains("wifi_pass")) {
        logObj["wifi_pass"] = "***";
    }

    if(logObj.contains("sessdata")) {
        logObj["sessdata"] = QString("*** length=%1 ***")
                .arg(obj.value("sessdata").toString().length());
    }

    QJsonDocument logDoc(logObj);
    appendLog("TX: " + QString::fromUtf8(logDoc.toJson(QJsonDocument::Compact)).trimmed());
}

void Widget::handleJsonLine(const QByteArray &line)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);

    if(err.error != QJsonParseError::NoError || !doc.isObject()) {
        appendLog("JSON解析失败：" + err.errorString());
        return;
    }

    QJsonObject obj = doc.object();

    lastJsonObj=obj;

    bool ok = obj.value("ok").toBool(false);
    QString msg = obj.value("msg").toString();

    lastJsonOk = ok;
    lastJsonMsg = msg;

    if(obj.contains("seq")) {
        lastChunkSeq = obj.value("seq").toInt(-1);
    }

    if(ok) {
        appendLog("设备返回成功：" + msg);
    } else {
        appendLog("设备返回失败：" + msg);
    }

    if(ok && msg.contains("rebooting")) {
        appendLog("设备配置已写入，正在重启。请等待 3~5 秒后重新连接。");

        if(serial->isOpen()) {
            serial->close();
        }

        ui->btnConnect->setText("连接设备");
        ui->comboPort->setEnabled(true);
        ui->comboBaud->setEnabled(true);
        setConfigButtonsEnabled(false);

        return;
    }

    /*
     * get_config 返回时，固件为了安全不会回传 wifi_pass 和 sessdata 原文，
     * 只回传 has_wifi_pass / has_sessdata。
     */
    if(obj.contains("configured")) {
        QString ssid = obj.value("wifi_ssid").toString();
        QString biliUid = obj.value("bili_uid").toString();
        QString latitude = obj.value("latitude").toString();
        QString longitude = obj.value("longitude").toString();

        ui->lineWifiSsid->setText(ssid);
        ui->lineBiliUid->setText(biliUid);
        ui->lineLatitude->setText(latitude);
        ui->lineLongitude->setText(longitude);

        bool hasPass = obj.value("has_wifi_pass").toBool(false);
        bool hasSessdata = obj.value("has_sessdata").toBool(false);

        if(hasPass) {
            ui->lineWifiPass->setPlaceholderText("设备中已保存密码；如需修改请重新输入");
            ui->lineWifiPass->clear();
        }

        if(hasSessdata) {
            ui->textSessdata->setPlaceholderText("设备中已保存 SESSDATA；如需修改请重新输入");
            ui->textSessdata->clear();
        }

        appendLog(QString("读取配置完成：SSID=%1, UID=%2, has_wifi_pass=%3, has_sessdata=%4")
                  .arg(ssid)
                  .arg(biliUid)
                  .arg(hasPass ? "true" : "false")
                  .arg(hasSessdata ? "true" : "false"));
    }
}

void Widget::flushEspLine()
{
    if(!serial->isOpen()) {
        return;
    }

    serial->write("\r\n\r\n\r\n");
    serial->waitForBytesWritten(1000);
    QThread::msleep(200);

    rxBuffer.clear();
    serial->clear(QSerialPort::Input);
}

void Widget::writeConfigToEsp32()
{
    QString ssid = ui->lineWifiSsid->text().trimmed();
    QString pass = ui->lineWifiPass->text();
    QString uid = ui->lineBiliUid->text().trimmed();
    QString sessdata = ui->textSessdata->toPlainText().trimmed();
    QString latitude = ui->lineLatitude->text().trimmed();
    QString longitude = ui->lineLongitude->text().trimmed();

    QString qrPath = ui->lineQR_code->text().trimmed();
    QString coverPath = ui->lineCover->text().trimmed();

    /*
     * 支持“部分写入”：
     * - 哪个输入框有内容，就只下发哪个字段；
     * - 空着的字段完全不出现在 JSON 里，ESP32 端会保留 NVS 里的旧值；
     * - 图片也可以单独写入，不再要求 WiFi 账号不能为空；
     * - 需要清空所有配置时，仍然使用“清空配置”按钮。
     */
    bool hasTextConfig = !ssid.isEmpty()
                      || !pass.isEmpty()
                      || !uid.isEmpty()
                      || !sessdata.isEmpty()
                      || !latitude.isEmpty()
                      || !longitude.isEmpty();

    bool hasImageConfig = !qrPath.isEmpty() || !coverPath.isEmpty();

    if(!hasTextConfig && !hasImageConfig) {
        QMessageBox::warning(this,
                             "提示",
                             "请至少填写一个配置项，或选择一张二维码 / 封面图片。");
        return;
    }

    if(!ssid.isEmpty() && ssid.toUtf8().size() >= 33) {
        QMessageBox::warning(this, "提示", "WiFi账号太长，ESP32端最大支持32字节。");
        return;
    }

    if(!pass.isEmpty() && pass.toUtf8().size() >= 65) {
        QMessageBox::warning(this, "提示", "WiFi密码太长，ESP32端最大支持64字节。");
        return;
    }

    if(!uid.isEmpty() && uid.toUtf8().size() >= 32) {
        QMessageBox::warning(this, "提示", "B站UID太长，ESP32端最大支持31字节。");
        return;
    }

    if(!sessdata.isEmpty() && sessdata.toUtf8().size() >= 1024) {
        QMessageBox::warning(this, "提示", "SESSDATA太长，ESP32端最大支持1023字节。");
        return;
    }

    if(!latitude.isEmpty() && latitude.toUtf8().size() >= 32) {
        QMessageBox::warning(this, "提示", "纬度太长，ESP32端最大支持31字节。");
        return;
    }

    if(!longitude.isEmpty() && longitude.toUtf8().size() >= 32) {
        QMessageBox::warning(this, "提示", "经度太长，ESP32端最大支持31字节。");
        return;
    }

    serial->clear(QSerialPort::Input);
    QThread::msleep(100);
    qApp->processEvents();

    flushEspLine();

    /*
     * 处理 flushEspLine 触发出来的旧残留日志 / bad json，
     * 然后再清状态，避免旧错误影响下一次等待。
     */
    QElapsedTimer drainTimer;
    drainTimer.start();
    while(drainTimer.elapsed() < 300) {
        qApp->processEvents();
        QThread::msleep(10);
    }

    rxBuffer.clear();
    serial->clear(QSerialPort::Input);

    lastJsonOk = false;
    lastJsonMsg.clear();
    lastJsonObj = QJsonObject();
    lastChunkSeq = -1;

    if(hasTextConfig) {
        QJsonObject obj;
        obj["cmd"] = "set_config";

        if(!ssid.isEmpty()) {
            obj["wifi_ssid"] = ssid;
        }

        if(!pass.isEmpty()) {
            obj["wifi_pass"] = pass;
        }

        if(!uid.isEmpty()) {
            obj["bili_uid"] = uid;
        }

        if(!sessdata.isEmpty()) {
            obj["sessdata"] = sessdata;
        }

        if(!latitude.isEmpty()) {
            obj["latitude"] = latitude;
        }

        if(!longitude.isEmpty()) {
            obj["longitude"] = longitude;
        }

        sendJson(obj);

        if(!waitForDeviceOk("config saved", 10000)) {
            appendLog("文字配置写入未确认，取消后续发送。");
            return;
        }

        appendLog("文字配置部分写入完成，未填写的项目已保留设备原值。");
    }
    else {
        appendLog("未填写文字配置，仅发送已选择的图片。");
    }

    sendImagesIfSelected();
}

void Widget::readConfigFromEsp32()
{
    QJsonObject obj;
    obj["cmd"] = "get_config";

    sendJson(obj);
}

void Widget::clearConfigOnEsp32()
{
    int ret = QMessageBox::question(this,
                                    "确认清空",
                                    "确定要清空 ESP32 中保存的配置吗？\n清空后设备会重启。");

    if(ret != QMessageBox::Yes) {
        return;
    }

    serial->clear(QSerialPort::Input);
    rxBuffer.clear();

    lastJsonOk = false;
    lastJsonMsg.clear();
    lastJsonObj = QJsonObject();
    lastChunkSeq = -1;

    QJsonObject obj;
    obj["cmd"] = "clear_config";

    sendJson(obj);

    /*
     * clear_config 成功后 ESP32 会回复 {"ok":true,"msg":"rebooting"} 并重启。
     * 等到这条回复后，handleJsonLine() 会自动关闭串口，避免用户在设备重启/重新枚举期间继续发送导致超时。
     */
    (void)waitForDeviceOk("rebooting", 5000);
}

void Widget::appendLog(const QString &text)
{
    QString now = QDateTime::currentDateTime().toString("hh:mm:ss");

    /*
     * 如果你的日志框不是 textLog，把这里改成你的 objectName。
     */
    ui->textLog->appendPlainText("[" + now + "] " + text);
}

void Widget::setConfigButtonsEnabled(bool enabled)
{
    ui->btnWriteConfig->setEnabled(enabled);
    ui->btnReadConfig->setEnabled(enabled);
    ui->btnClearConfig->setEnabled(enabled);
}

void Widget::chooseQrCodeImage()
{
    QString path = QFileDialog::getOpenFileName(
                this,
                "选择 B站二维码图片",
                QString(),
                "Images (*.png *.jpg *.jpeg *.bmp)");

    if(path.isEmpty()) {
        return;
    }

    ui->lineQR_code->setText(path);
    appendLog("已选择 QR_code 图片：" + path);
}

void Widget::chooseCoverImage()
{
    QString path = QFileDialog::getOpenFileName(
                this,
                "选择封面图片",
                QString(),
                "Images (*.png *.jpg *.jpeg *.bmp)");

    if(path.isEmpty()) {
        return;
    }

    ui->lineCover->setText(path);
    appendLog("已选择 cover 图片：" + path);
}

QByteArray Widget::imageToRgb565(const QString &path, int w, int h)
{
    QImage img(path);

    if(img.isNull()) {
        appendLog("图片打开失败：" + path);
        return QByteArray();
    }

    img = img.convertToFormat(QImage::Format_RGB888);
    img = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QByteArray out;
    out.resize(w * h * 2);

    int index = 0;

    for(int y = 0; y < h; ++y) {
        const uchar *line = img.constScanLine(y);

        for(int x = 0; x < w; ++x) {
            int r = line[x * 3 + 0];
            int g = line[x * 3 + 1];
            int b = line[x * 3 + 2];

            quint16 rgb565 =
                    ((r & 0xF8) << 8) |
                    ((g & 0xFC) << 3) |
                    ((b & 0xF8) >> 3);

            /*
             * 小端 RGB565：
             * 和你之前 LVGL 转出来的 cover.c 低字节在前一致。
             */
            out[index++] = (char)(rgb565 & 0xFF);
            out[index++] = (char)((rgb565 >> 8) & 0xFF);
        }
    }

    appendLog(QString("图片转换完成：%1 -> %2x%3 RGB565, %4 字节")
              .arg(QFileInfo(path).fileName())
              .arg(w)
              .arg(h)
              .arg(out.size()));

    return out;
}

bool Widget::sendImageToEsp32(const QString &name,
                              const QByteArray &rgb565,
                              int w,
                              int h)
{
    if(!serial->isOpen()) {
        QMessageBox::warning(this, "提示", "请先连接串口。");
        return false;
    }

    if(rgb565.isEmpty()) {
        appendLog("图片数据为空，取消发送：" + name);
        return false;
    }

    QJsonObject begin;
    begin["cmd"] = "img_begin";
    begin["name"] = name;
    begin["w"] = w;
    begin["h"] = h;
    begin["format"] = "rgb565";
    begin["size"] = rgb565.size();

    appendLog(QString("开始发送图片：%1, size=%2").arg(name).arg(rgb565.size()));

    sendJson(begin);

    if(!waitForDeviceOk("img_begin ok", 30000,name)) {
        appendLog("img_begin 没有成功，取消发送：" + name);
        return false;
    }

    QThread::msleep(30);
    qApp->processEvents();

    /*
     * 图片分片不能太大。
     * ESP32-S3 USB Serial/JTAG 默认 RX 缓冲较小，大 JSON 一次写入容易被截断，
     * 表现为 seq=1 超时、bad json 或 chunk offset mismatch。
     * 这里降低到 240 字节，配合 ESP32 端 4096 字节 USB RX 缓冲后传输会稳定很多。
     */
    const int chunkSize = 240;
    int seq = 0;
    int total = (rgb565.size() + chunkSize - 1) / chunkSize;

    for(int offset = 0; offset < rgb565.size(); offset += chunkSize) {
        QByteArray chunk = rgb565.mid(offset, chunkSize);

        QJsonObject obj;
        obj["cmd"] = "img_chunk";
        obj["name"] = name;
        obj["seq"] = seq;
        obj["offset"] = offset;
        obj["data"] = QString::fromLatin1(chunk.toBase64());

        sendJson(obj);

        if(!waitForChunkOk(name, seq, 10000)) {
            appendLog(QString("分片发送失败，停止：%1 seq=%2").arg(name).arg(seq));
            return false;
        }

        /*
         * 给 USB Serial/JTAG 和 ESP32 JSON 解析任务一点喘息时间，
         * 避免 PC 端连续写入把设备端 RX 缓冲顶满。
         */
        QThread::msleep(15);
        qApp->processEvents();

        seq++;

        appendLog(QString("发送 %1 分片：%2/%3")
                  .arg(name)
                  .arg(seq)
                  .arg(total));
    }

    QJsonObject end;
    end["cmd"] = "img_end";
    end["name"] = name;
    end["size"] = rgb565.size();

    sendJson(end);

    if(!waitForDeviceOk("image saved", 60000, name)) {
        appendLog("img_end 没有成功：" + name);
        return false;
    }

    appendLog("图片发送完成：" + name);
    return true;
}

void Widget::sendImagesIfSelected()
{
    QString qrPath = ui->lineQR_code->text().trimmed();
    QString coverPath = ui->lineCover->text().trimmed();

    bool hasImage = false;

    if(!qrPath.isEmpty()) {
        hasImage = true;

        QByteArray qrData = imageToRgb565(qrPath, 74, 74);

        if(qrData.size() != 74 * 74 * 2) {
            QMessageBox::warning(this, "错误", "QR_code 图片转换失败。");
            return;
        }

        if(!sendImageToEsp32("qr_code", qrData, 74, 74)) {
            appendLog("QR_code 发送失败，停止后续图片发送，不执行重启。");
            return;
        }
    }

    if(!coverPath.isEmpty()) {
        hasImage = true;

        QByteArray coverData = imageToRgb565(coverPath, 240, 240);

        if(coverData.size() != 240 * 240 * 2) {
            QMessageBox::warning(this, "错误", "cover 图片转换失败。");
            return;
        }

        if(!sendImageToEsp32("cover", coverData, 240, 240)) {
            appendLog("cover 发送失败，停止重启。");
            return;
        }
    }

    if(!hasImage) {
        appendLog("未选择图片，仅写入文字配置，准备重启设备使配置生效。");
    }

    QJsonObject reboot;
    reboot["cmd"] = "reboot";
    sendJson(reboot);
}

bool Widget::waitForDeviceOk(const QString &expectMsg,
                             int timeoutMs,
                             const QString &expectName)
{
    lastJsonOk = false;
    lastJsonMsg.clear();
    lastJsonObj = QJsonObject();

    QElapsedTimer timer;
    timer.start();

    while(timer.elapsed() < timeoutMs) {
        qApp->processEvents();

        if(lastJsonOk && lastJsonMsg.contains(expectMsg)) {
            if(expectName.isEmpty() ||
               lastJsonObj.value("name").toString() == expectName) {
                return true;
            }
        }

        if(!lastJsonMsg.isEmpty() && !lastJsonOk) {
            appendLog("等待设备响应失败：" + lastJsonMsg);
            return false;
        }

        QThread::msleep(10);
    }

    appendLog("等待设备响应超时：" + expectMsg +
              (expectName.isEmpty() ? "" : (" name=" + expectName)));
    return false;
}

bool Widget::waitForChunkOk(const QString &expectName, int seq, int timeoutMs)
{
    lastJsonOk = false;
    lastJsonMsg.clear();
    lastJsonObj = QJsonObject();
    lastChunkSeq = -1;

    QElapsedTimer timer;
    timer.start();

    while(timer.elapsed() < timeoutMs) {
        qApp->processEvents();

        if(lastJsonOk &&
           lastJsonMsg.contains("chunk ok") &&
           lastChunkSeq == seq &&
           lastJsonObj.value("name").toString() == expectName) {
            return true;
        }

        if(!lastJsonMsg.isEmpty() && !lastJsonOk) {
            appendLog(QString("等待分片 ACK 失败：name=%1 seq=%2, msg=%3")
                      .arg(expectName)
                      .arg(seq)
                      .arg(lastJsonMsg));
            return false;
        }

        QThread::msleep(10);
    }

    appendLog(QString("等待分片 ACK 超时：name=%1 seq=%2")
              .arg(expectName)
              .arg(seq));
    return false;
}

