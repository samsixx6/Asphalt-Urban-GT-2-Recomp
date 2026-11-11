"""Package/sign the personal ARM64 test APK with stock Android build tools.

The owned ISO stays outside the APK. No Gradle, machine-wide JDK, user signing
key, Android emulator or attached phone is required by this build step.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/android'
TOOLS = ROOT / 'toolchains/android'
JDK = TOOLS / 'jdk/bin'
BT = TOOLS / 'sdk/build-tools/35.0.0'
ANDROID = TOOLS / 'sdk/platforms/android-35/android.jar'
TC = TOOLS / 'ndk/toolchains/llvm/prebuilt/windows-x86_64/bin'
SOURCE = ROOT / 'android/app/src/main'
IMAGE_SHA = '6e52686f5f0b9e476a2481661b91ae241bd7b429e060f197d9728565a6317c3b'
ISO_SHA = '64ab1ba788baade948b9b89d47145a653c063e85afed403b89d917d8a8703ec4'
BOOT_SHA = 'd2a2e78ae0a19b4d7ac2eeb6386b8ae16cd7c35c624bdd59d0c6b4d241b31f9c'
CONTROL_IMAGES = {
    'NOS.png': 'ae9ae48d8e5a0b00c67239dc42dd8af21db40ae8cd159680b0d2f67a29c112f3',
    'Brake.png': '7cd815757e82ad79d674c90d999b34adc4ff754a722d8a31162215bebee4740c',
    'Pause.png': '577f584e90aff3f89b9326a10aec650ba3f13396733f503e1798fe962e4cb4f8',
    'Camera.png': 'fe962d4d4dc2122bf88ce5607d4bc4587f4d2dc419a707f85e5272e4d2ec4175',
}
APP_ICON_SHA = '274a6f628bcbbfbd8119ff8a9e7eea68c8320456ebb28dc75f31dcbf8033c97b'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def run(label, args):
    result = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True, timeout=300)
    output = (result.stdout + result.stderr).decode(errors='replace')
    (BUILD / (label + '.log')).write_text(output, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(label + ':\n' + output[-10000:])
    print(label + ': passed', flush=True)
    return output


def clean_child(name):
    path = (BUILD / name).resolve()
    assert path.parent == BUILD.resolve() and name in ('apk-classes', 'apk-dex', 'apk-res')
    if path.exists():
        shutil.rmtree(path)
    path.mkdir()
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, help='Supported original ISO; defaults to the legacy parent-folder filename')
    options = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    assert sha(SOURCE / 'res/drawable-nodpi/app_icon.png') == APP_ICON_SHA, 'Supplied app icon differs'
    image = ROOT / 'work/input/ASPHALT_REBASED.ELF'
    iso = options.iso if options.iso else ROOT.parent / 'Asphalt - Urban GT 2 (Europe) (En,Fr,De,Es,It).iso'
    assert iso.stat().st_size == 884146176 and sha(iso) == ISO_SHA, 'Original ISO differs'
    assert sha(ROOT / 'work/input/BOOT.BIN') == BOOT_SHA, 'Original BOOT differs'
    assert image.stat().st_size == 4343022 and sha(image) == IMAGE_SHA, 'Startup image differs'
    library = BUILD / 'libasphalt.so'
    native = json.loads((ROOT / 'build/android-native-results.json').read_text())
    assert sha(library) == native['sha256'] and native['abi'] == 'arm64-v8a'
    assert native.get('hud_diagnostics') is True, 'Motion build retains bounded automatic HUD observations'
    motion_sources = ['src/native_engine.c', 'src/native_camera.c', 'src/native_camera.h',
                      'src/display_hooks.c', 'src/display-hooks.json', 'src/display_semantic_hooks.c']
    assert native.get('motion_source_sha256') == {name: sha(ROOT / name) for name in motion_sources}, 'Native library must be rebuilt from the current motion sources'
    packaged_library = BUILD / 'libasphalt-packaged.so'
    run('strip', [TC / 'llvm-strip.exe', '--strip-unneeded', '-o', packaged_library, library])
    details = run('packaged-elf', [TC / 'llvm-readelf.exe', '-h', '-l', '-d', '-Ws', packaged_library])
    assert 'AArch64' in details and '0x4000' in details
    for entry in ('Start', 'Input', 'Stop', 'Status', 'DisplayRestartRequested', 'DisplayRecovered', 'DisplayInsets', 'PresentedFrames', 'RaceControlsActive', 'RaceControlsHidden', 'FPSCounterEnabled', 'UILayout', 'SetResumed', 'SetSurface', 'RecoverSaves', 'ValidateSaves'):
        assert 'Java_com_samsixx_asphalt2recomp_GameActivity_native' + entry in details
    # All PT_LOAD segments, not only one incidental 0x4000 string, must be 16 KB aligned.
    loads = [line.split() for line in details.splitlines() if line.strip().startswith('LOAD ')]
    assert loads and all(int(row[-1], 16) >= 16384 for row in loads)
    classes = clean_child('apk-classes')
    dex = clean_child('apk-dex')
    java = sorted((SOURCE / 'java').rglob('*.java'))
    run('javac', [JDK / 'javac.exe', '--release', '8', '-classpath', ANDROID, '-d', classes, *java])
    run('d8', [JDK / 'java.exe', '-cp', BT / 'lib/d8.jar', 'com.android.tools.r8.D8',
               '--lib', ANDROID, '--min-api', '26', '--output', dex, *sorted(classes.rglob('*.class'))])
    resource_args = []
    if (SOURCE / 'res').is_dir() and list((SOURCE / 'res').rglob('*.*')):
        resources = clean_child('apk-res')
        run('resources', [BT / 'aapt2.exe', 'compile', '--no-crunch', '--dir', SOURCE / 'res', '-o', resources])
        resource_args = sorted(resources.glob('*.flat'))
    base = BUILD / 'base.apk'
    run('manifest', [BT / 'aapt2.exe', 'link', '-I', ANDROID, '--manifest', SOURCE / 'AndroidManifest.xml',
                     '--min-sdk-version', '26', '--target-sdk-version', '35', '-o', base, *resource_args])
    unsigned = BUILD / 'unsigned.apk'
    with zipfile.ZipFile(base) as original, zipfile.ZipFile(unsigned, 'w') as apk:
        for info in original.infolist():
            apk.writestr(info, original.read(info))
        for path in sorted(dex.glob('*.dex')):
            apk.write(path, path.name, compress_type=zipfile.ZIP_DEFLATED)
        # extractNativeLibs=false: store and page-align the ELF for direct loading.
        apk.write(packaged_library, 'lib/arm64-v8a/libasphalt.so', compress_type=zipfile.ZIP_STORED)
        apk.write(image, 'assets/asphalt-image.elf', compress_type=zipfile.ZIP_DEFLATED)
        for name, digest in CONTROL_IMAGES.items():
            path = SOURCE / 'assets/controls' / name
            assert sha(path) == digest, 'Supplied control icon differs: ' + name
            apk.write(path, 'assets/controls/' + name, compress_type=zipfile.ZIP_DEFLATED)
        for path, name in [(ROOT / 'THIRD_PARTY_LICENSES.txt', 'THIRD_PARTY_LICENSES.txt'),
                           (ROOT / 'vendor/psprecomp/LICENSE', 'psprecomp-MIT.txt'),
                           (TOOLS / 'ffmpeg-7.1.4/COPYING.LGPLv2.1', 'FFmpeg-LGPLv2.1.txt')]:
            apk.write(path, 'assets/licenses/' + name, compress_type=zipfile.ZIP_DEFLATED)
    aligned = BUILD / 'aligned.apk'
    run('zipalign', [BT / 'zipalign.exe', '-P', '16', '-f', '4', unsigned, aligned])
    key = BUILD / 'test-signing.keystore'
    if not key.exists():
        run('keytool', [JDK / 'keytool.exe', '-genkeypair', '-keystore', key,
                        '-storepass', 'android', '-keypass', 'android', '-alias', 'androiddebugkey',
                        '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
                        '-dname', 'CN=Asphalt 2 Personal Test, O=Personal Build, C=PK'])
    final = BUILD / 'Asphalt-Urban-GT-2-v1.0-arm64.apk'
    run('sign', [JDK / 'java.exe', '-jar', BT / 'lib/apksigner.jar', 'sign', '--ks', key,
                 '--ks-key-alias', 'androiddebugkey', '--ks-pass', 'pass:android',
                 '--key-pass', 'pass:android', '--out', final, aligned])
    signature = run('signature', [JDK / 'java.exe', '-jar', BT / 'lib/apksigner.jar',
                                  'verify', '--verbose', '--print-certs', final])
    key_certificate = subprocess.check_output([str(JDK / 'keytool.exe'), '-exportcert',
        '-keystore', str(key), '-storepass', 'android', '-alias', 'androiddebugkey'])
    signer_sha = hashlib.sha256(key_certificate).hexdigest()
    assert ('Signer #1 certificate SHA-256 digest: ' + signer_sha) in signature, 'Signing identity changed'
    run('alignment', [BT / 'zipalign.exe', '-c', '-P', '16', '-v', '4', final])
    badging = run('badging', [BT / 'aapt2.exe', 'dump', 'badging', final])
    assert "package: name='com.samsixx.asphalt2recomp'" in badging
    assert "versionCode='33'" in badging and "versionName='1.0'" in badging
    assert "application-label:'Asphalt Urban GT 2'" in badging
    assert "icon='res/drawable-nodpi-v4/app_icon.png'" in badging
    assert "minSdkVersion:'26'" in badging and "targetSdkVersion:'35'" in badging
    assert "uses-permission: name='android.permission.MANAGE_EXTERNAL_STORAGE'" in badging
    assert "native-code: 'arm64-v8a'" in badging
    assert "launchable-activity: name='com.samsixx.asphalt2recomp.LauncherActivity'" in badging
    with zipfile.ZipFile(final) as apk:
        names = apk.namelist()
        assert not any(name.lower().endswith(('.iso', '.chd', '.cso')) for name in names)
        assert hashlib.sha256(apk.read('assets/asphalt-image.elf')).hexdigest() == IMAGE_SHA
        assert hashlib.sha256(apk.read('lib/arm64-v8a/libasphalt.so')).hexdigest() == sha(packaged_library)
        assert apk.getinfo('lib/arm64-v8a/libasphalt.so').compress_type == zipfile.ZIP_STORED
        assert 'classes.dex' in names
        for name, digest in CONTROL_IMAGES.items():
            assert hashlib.sha256(apk.read('assets/controls/' + name)).hexdigest() == digest
        assert hashlib.sha256(apk.read('res/drawable-nodpi-v4/app_icon.png')).hexdigest() == APP_ICON_SHA
    report = {'version_code': 33, 'version_name': '1.0', 'signer_sha256': signer_sha, 'apk': str(final), 'sha256': sha(final), 'bytes': final.stat().st_size,
              'package': 'com.samsixx.asphalt2recomp', 'label': 'Asphalt Urban GT 2',
              'abi': 'arm64-v8a', 'min_api': 26, 'target_api': 35,
              'native_unstripped_sha256': sha(library), 'native_packaged_sha256': sha(packaged_library),
              'motion_source_sha256': native['motion_source_sha256'],
              'startup_image_sha256': IMAGE_SHA, 'owned_iso_included': False,
              'original_iso_verified': True, 'original_boot_verified': True,
              'elf_load_alignment': 16384, 'apk_alignment_verified': True,
              'signature_verified': True, 'control_icon_sha256': CONTROL_IMAGES, 'app_icon_sha256': APP_ICON_SHA, 'phone_runtime_tested': False,
              'hud_diagnostics': native['hud_diagnostics'], 'hud_runtime_identity_case_count': 42, 'motion_fps_cause_confirmed': True, 'camera_alias_case_count': 3, 'civilian_current_pose_verified': True, 'civilian_road_alignment_verified': True, 'loading_touch_hide_verified': True, 'factory_defaults': [2,1,60,1,0,1], 'phone_runtime_tested_by_agent': False}
    (ROOT / 'build/android-apk-results.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
