"""Keep the application media runtime independent from the optional encoder."""
from pathlib import Path
import re
import shutil


FFMPEG = re.compile(
    r'(?:ffmpeg|ffprobe)(?:\.exe)?$|ffmpegmediaplugin|'
    r'(?:lib)?(?:avcodec|avdevice|avfilter|avformat|avutil|postproc|swresample|swscale)[.\d-]',
    re.IGNORECASE,
)


def without_ffmpeg(root):
    for path in root.rglob('*'):
        if FFMPEG.search(path.name):
            raise RuntimeError(f'FFmpeg belongs in the optional replay plugin, not the installer: {path}')


def deploy_replay_plugins(source, destination, major, windows):
    source = Path(source)
    if windows:
        media = 'mediaservice/wmfengine.dll' if major == 5 else 'multimedia/windowsmediaplugin.dll'
        image = 'imageformats/qwebp.dll'
    else:
        media = 'mediaservice/libqavfmediaplayer.dylib' if major == 5 else 'multimedia/libdarwinmediaplugin.dylib'
        image = 'imageformats/libqwebp.dylib'
    for relative in (media, image):
        target = destination / relative
        if target.is_file():
            continue
        plugin = source / relative
        if not plugin.is_file():
            raise RuntimeError(f'The target Qt installation is missing {relative}')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(plugin, target)
