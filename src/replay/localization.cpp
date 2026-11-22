#include "localization.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace Replay {
namespace {
class Translator final : public QTranslator {
public:
  using QTranslator::QTranslator;

  bool isEmpty() const override { return false; }

  QString translate(const char *context, const char *sourceText, const char *, int) const override {
    if (qstrcmp(context, "OfflineReplay") != 0) return {};
    const auto language = QSettings().value("appearance/language", "system").toString();
    if (language != "zh" &&
        !(language == "system" && QLocale::system().language() == QLocale::Chinese)) {
      return {};
    }
    static const QHash<QByteArray, QString> translations{
        {"Invalid or unsupported asciinema recording.", QStringLiteral("无效或不受支持的终端录像。")},
        {"The FFmpeg plugin is incomplete. Download it again in Settings.",
         QStringLiteral("转码插件不完整，请在设置中重新下载。")},
        {"FFmpeg checksum verification failed. Download it again in Settings.",
         QStringLiteral("转码插件校验失败，请在设置中重新下载。")},
        {"The FFmpeg plugin reported an invalid version.", QStringLiteral("转码插件返回了无效版本。")},
        {"The FFmpeg plugin requires FFmpeg 6.0 or newer.",
         QStringLiteral("转码插件需要 FFmpeg 6.0 或更高版本。")},
        {"Cannot reset the interrupted FFmpeg download.", QStringLiteral("无法重置中断的转码插件下载。")},
        {"Cannot prepare the downloaded FFmpeg binary.", QStringLiteral("无法准备已下载的转码插件文件。")},
        {"The FFmpeg download was redirected to an untrusted host.",
         QStringLiteral("转码插件下载被重定向到不受信任的主机。")},
        {"Cannot resolve the configured proxy.", QStringLiteral("无法解析当前代理配置。")},
        {"The FFmpeg server returned an invalid resume range.",
         QStringLiteral("插件下载服务器返回了无效的断点续传范围。")},
        {"The FFmpeg server returned an unexpected file size.",
         QStringLiteral("插件下载服务器返回的文件大小异常。")},
        {"The FFmpeg download exceeded its expected size.",
         QStringLiteral("插件下载文件超过预期大小。")},
        {"The FFmpeg download timed out.", QStringLiteral("插件下载超时。")},
        {"Cannot download FFmpeg: %1. Retry to continue.",
         QStringLiteral("无法下载转码插件：%1。请重试以继续。")},
        {"The FFmpeg download is incomplete. Try again to continue.",
         QStringLiteral("转码插件下载未完成，请重试以继续。")},
        {"The downloaded FFmpeg binary failed checksum verification.",
         QStringLiteral("下载的转码插件文件校验失败。")},
        {"Too many redirects while downloading FFmpeg.", QStringLiteral("转码插件下载重定向次数过多。")},
        {"The FFmpeg plugin cannot run or does not provide the H.264 encoder.",
         QStringLiteral("转码插件无法运行，或不支持 H.264 编码。")},
        {"The FFmpeg plugin cannot run on this system.", QStringLiteral("转码插件无法在当前系统运行。")},
        {"The plugin's ffprobe cannot run on this system.",
         QStringLiteral("转码插件的 ffprobe 无法在当前系统运行。")},
        {"The FFmpeg and ffprobe versions do not match.",
         QStringLiteral("转码插件中的 FFmpeg 与 ffprobe 版本不一致。")},
        {"Cannot write FFmpeg plugin metadata.", QStringLiteral("无法写入转码插件元数据。")},
        {"Download the FFmpeg plugin in Settings first.", QStringLiteral("请先在设置中下载转码插件。")},
        {"No FFmpeg download is available for this platform.",
         QStringLiteral("当前平台没有可下载的转码插件。")},
        {"Cannot create the FFmpeg plugin directory.", QStringLiteral("无法创建转码插件目录。")},
        {"An FFmpeg download is already in progress.", QStringLiteral("转码插件正在下载，请稍后再试。")},
        {"Cannot create the FFmpeg download directory.", QStringLiteral("无法创建转码插件下载目录。")},
        {"Cannot stage the FFmpeg plugin locally.", QStringLiteral("无法在本机准备转码插件。")},
        {"Cannot stage the downloaded FFmpeg plugin.", QStringLiteral("无法在本机准备已下载的转码插件。")},
        {"No FFmpeg plugin is available for this platform.", QStringLiteral("当前平台没有可用的转码插件。")},
        {"The FFmpeg plugin is busy. Try again later.", QStringLiteral("转码插件正忙，请稍后再试。")},
        {"Invalid plugin checksum file.", QStringLiteral("插件校验文件无效。")},
        {"The plugin checksum file does not match this platform.",
         QStringLiteral("插件校验文件与当前平台不匹配。")},
        {"The plugin contains an invalid binary size.", QStringLiteral("插件包含无效的文件大小。")},
        {"The plugin labeled as FFmpeg 6.0 does not match the trusted build.",
         QStringLiteral("插件标称版本为 FFmpeg 6.0，但与可信构建不匹配。")},
        {"The FFmpeg process timed out.", QStringLiteral("转码插件执行超时。")},
        {"Invalid RDP/VNC recording drawing data.", QStringLiteral("RDP/VNC 录像绘制数据无效。")},
        {"The RDP/VNC recording renderer exceeded its resource limit.",
         QStringLiteral("RDP/VNC 录像渲染超出资源限制。")},
        {"Unsupported RDP/VNC recording composition mask: %1",
         QStringLiteral("不支持的 RDP/VNC 录像合成模式：%1。")},
        {"The RDP/VNC protocol recording uses an unsupported image format.",
         QStringLiteral("RDP/VNC 协议录像使用了不支持的图像格式。")},
        {"Cannot decode a RDP/VNC recording %1 image; check the Qt image plugin.",
         QStringLiteral("无法解码 RDP/VNC 录像中的 %1 图像，请检查 Qt 图像插件。")},
        {"Unsupported RDP/VNC recording instruction: %1",
         QStringLiteral("不支持的 RDP/VNC 录像指令：%1。")},
        {"Invalid or incomplete replay archive.", QStringLiteral("录像归档无效或不完整。")},
        {"Invalid or truncated RDP/VNC recording instruction.",
         QStringLiteral("RDP/VNC 录像指令无效或已截断。")},
        {"Only self-contained local MP4 video is supported.", QStringLiteral("仅支持自包含的本地 MP4 视频。")},
        {"This archive does not contain a supported offline recording.",
         QStringLiteral("该归档不包含受支持的离线录像。")},
        {"Unsupported recording. For RDP/VNC protocol fragments, open the complete offline TAR.",
         QStringLiteral("不支持的录像格式。对于 RDP/VNC 协议分片，请打开完整的离线 TAR 文件。")},
        {"The selected replay is not a supported local file or exceeds the size limit.",
         QStringLiteral("所选录像不是受支持的本地文件，或文件大小超过限制。")},
        {"The fragment does not contain its initial screen. Open the complete offline TAR.",
         QStringLiteral("当前分片不包含初始画面，请打开完整的离线 TAR。")},
        {"Cannot reconcile the RDP/VNC recording part timestamps.",
         QStringLiteral("无法校正 RDP/VNC 录像各分段的时间戳。")},
        {"The RDP/VNC protocol recording timeline is invalid or exceeds 24 hours.",
         QStringLiteral("RDP/VNC 协议录像时间轴无效或超过 24 小时。")},
        {"No display frames were found in the RDP/VNC protocol recording.",
         QStringLiteral("RDP/VNC 协议录像中未找到可显示画面。")},
        {"Canceled.", QStringLiteral("已取消。")},
        {"Cannot write the local replay file: %1", QStringLiteral("无法写入本地录像文件：%1。")},
        {"Cannot start gzip decompression.", QStringLiteral("无法启动 gzip 解压。")},
        {"Truncated gzip data.", QStringLiteral("gzip 数据不完整。")},
        {"The decompressed file exceeds the size limit.", QStringLiteral("解压后的文件超过大小限制。")},
        {"Invalid gzip member.", QStringLiteral("gzip 成员无效。")},
        {"Invalid or truncated gzip data.", QStringLiteral("gzip 数据无效或已截断。")},
        {"The terminal recording exceeds the size limit.", QStringLiteral("终端录像超过大小限制。")},
        {"Cannot create a private replay directory.", QStringLiteral("无法创建临时录像目录。")},
        {"The plugin archive exceeds the size limit.", QStringLiteral("插件包超过大小限制。")},
        {"The plugin archive must contain exactly the required files.",
         QStringLiteral("插件包必须且只能包含所需文件。")},
        {"The plugin archive is missing a required file.", QStringLiteral("插件包缺少必需文件。")},
        {"The plugin checksum file is too large.", QStringLiteral("插件校验文件过大。")},
        {"The exported video did not pass ffprobe validation.", QStringLiteral("导出的视频未通过 ffprobe 校验。")},
        {"The output already exists. Choose another file name.", QStringLiteral("输出文件已存在，请更换文件名。")},
        {"Cannot save the verified video in the selected folder (%1).",
         QStringLiteral("无法将校验后的视频保存到所选目录（%1）。")},
        {"Only RDP/VNC protocol recordings can be exported as MP4.",
         QStringLiteral("仅 RDP/VNC 协议录像可以导出为 MP4。")},
        {"Open the complete offline TAR to export this recording.",
         QStringLiteral("请打开完整的离线 TAR 文件后导出该录像。")},
        {"The original resolution is too large. Select a lower output resolution.",
         QStringLiteral("原始分辨率过大，请选择较低的输出分辨率。")},
        {"The FFmpeg plugin is busy. Try again after its current task finishes.",
         QStringLiteral("转码插件正忙，请等待当前任务结束后重试。")},
        {"Cannot start FFmpeg: %1", QStringLiteral("无法启动 FFmpeg：%1。")},
        {"Cannot allocate the video canvas.", QStringLiteral("无法分配视频画布内存。")},
        {"FFmpeg stopped accepting frames.", QStringLiteral("FFmpeg 已停止接收画面数据。")},
        {"FFmpeg failed while encoding. Check the output folder and available disk space.",
         QStringLiteral("FFmpeg 编码失败，请检查输出目录和可用磁盘空间。")},
        {"FFmpeg did not finish the MP4 in time.", QStringLiteral("FFmpeg 未能在规定时间内完成 MP4。")},
        {"FFmpeg could not finish the MP4. Check the output folder and available disk space.",
         QStringLiteral("FFmpeg 无法完成 MP4，请检查输出目录和可用磁盘空间。")},
    };
    return translations.value(QByteArray(sourceText));
  }
};

constexpr auto translatorProperty = "offlineReplayTranslatorInstalled";
} // namespace

void installTranslations() {
  auto *app = QCoreApplication::instance();
  if (!app || app->property(translatorProperty).toBool()) return;
  auto *translator = new Translator(app);
  if (app->installTranslator(translator)) {
    app->setProperty(translatorProperty, true);
  } else {
    delete translator;
  }
}

} // namespace Replay
