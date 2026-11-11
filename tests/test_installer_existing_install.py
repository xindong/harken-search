"""Exercise shipping Inno directory/occupancy policy in private fixtures.

Uses a unique per-user AppId, harmless payload, and an owned cmd.exe copy.
No Harken registration, shortcuts, legacy uninstall, or user data is touched.
Logs are retained under --artifact-root (7-day QA retention).
"""
from pathlib import Path
from datetime import date
import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = Path(tempfile.gettempdir()) / 'Harken-QA' / (
    date.today().isoformat() + '-installer-existing-install')


def find_iscc():
    configured = os.environ.get('INNO_SETUP_COMPILER')
    if configured:
        return Path(configured)
    on_path = shutil.which('ISCC.exe')
    if on_path:
        return Path(on_path)
    for base in (os.environ.get('LOCALAPPDATA'), os.environ.get('ProgramFiles(x86)'),
                 os.environ.get('ProgramFiles')):
        if base:
            candidate = Path(base) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'
            if candidate.is_file():
                return candidate
            candidate = Path(base) / 'Inno Setup 6' / 'ISCC.exe'
            if candidate.is_file():
                return candidate
    return Path('ISCC.exe')


ISCC = find_iscc()


@unittest.skipUnless(sys.platform == 'win32', 'Windows installer integration')
class InstallerExistingInstallTest(unittest.TestCase):
    def setUp(self):
        import winreg
        self.winreg = winreg
        self.run_dir = (ARTIFACT_ROOT / uuid.uuid4().hex).resolve()
        self.run_dir.mkdir(parents=True)
        self.app_id = 'HRTest_' + uuid.uuid4().hex
        self.key = chr(92).join(('Software', 'Microsoft', 'Windows', 'CurrentVersion', 'Uninstall',
                                self.app_id + '_is1'))
        self.install_dirs = set()
        self.invocation = 0
        self.addCleanup(self.uninstall_fixture)
        self.assertTrue(ISCC.is_file(), 'Inno Setup compiler is required')
        product = (ROOT / 'installer/HarkenSearch_Setup.iss').read_text(encoding='utf-8')
        # Execute actual policy functions, not a Python reimplementation.
        helpers = product[product.index('function CreateFileW'):product.index('function IsUserProfileInstallDir')]
        prepare = re.search('function PrepareToInstall.*?' + chr(10) + 'end;', product, re.S)
        code = helpers + (chr(10) + prepare.group(0) if prepare else '')
        setup = product.split('[Setup]', 1)[1].split('[Languages]', 1)[0]
        policy_keys = ('UsePreviousAppDir', 'CloseApplications', 'RestartApplications',
                       'ArchitecturesInstallIn64BitMode')
        policies = chr(10).join(line for line in setup.splitlines()
                             if any(line.startswith(key + '=') for key in policy_keys))
        messages = product.split('[CustomMessages]', 1)[1].split('[Code]', 1)[0]
        messages = chr(10).join(line.removeprefix('english.') for line in messages.splitlines()
                             if line.startswith('english.'))
        (self.run_dir / 'payload.txt').write_text('new application payload', encoding='utf-8')
        self.default_dir = self.run_dir / 'default-install'
        script = f'''[Setup]
AppId={self.app_id}
AppName=Harken Installer Regression Fixture
AppVersion=1
DefaultDirName={self.default_dir}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={self.run_dir}
OutputBaseFilename=fixture_setup
Compression=none
{policies}
[Files]
Source: "{self.run_dir / 'payload.txt'}"; DestDir: "{{app}}"; Flags: ignoreversion
[CustomMessages]
{messages}
[Code]
{code}
'''
        script_path = self.run_dir / 'fixture.iss'
        script_path.write_text(script, encoding='utf-8-sig')
        compiled = subprocess.run([str(ISCC), str(script_path)], capture_output=True, timeout=60)
        (self.run_dir / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
        self.assertEqual(compiled.returncode, 0, compiled.stdout.decode(errors='replace')[-3000:])
        self.setup_exe = self.run_dir / 'fixture_setup.exe'

    def install(self, directory=None):
        self.invocation += 1
        args = [str(self.setup_exe), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                '/LOG=' + str(self.run_dir / f'install-{self.invocation}.log')]
        if directory is not None:
            directory = directory.resolve()
            self.assertTrue(directory.is_relative_to(self.run_dir))
            self.install_dirs.add(directory)
            args.append('/DIR=' + str(directory))
        self.install_dirs.add(self.default_dir)
        return subprocess.run(args, timeout=45, creationflags=subprocess.CREATE_NO_WINDOW).returncode

    def registered_directory(self):
        with self.winreg.OpenKey(self.winreg.HKEY_CURRENT_USER, self.key, 0,
                                self.winreg.KEY_READ | self.winreg.KEY_WOW64_64KEY) as key:
            return Path(self.winreg.QueryValueEx(key, 'InstallLocation')[0]).resolve()

    def uninstall_fixture(self):
        # Only uninstallers created in this run's checked private tree.
        # Unlisted files (data sentinel and owned process image) are retained.
        for directory in self.install_dirs:
            self.assertTrue(directory.resolve().is_relative_to(self.run_dir))
            uninstaller = directory / 'unins000.exe'
            if uninstaller.is_file():
                result = subprocess.run([str(uninstaller), '/VERYSILENT', '/SUPPRESSMSGBOXES',
                                         '/NORESTART', '/LOG=' + str(directory / 'uninstall.log')],
                                        timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
                self.assertEqual(result.returncode, 0, 'Private fixture uninstall failed')

    def test_first_install_uses_default_location(self):
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.registered_directory(), self.default_dir)
        self.assertTrue((self.default_dir / 'payload.txt').is_file())

    def test_upgrade_defaults_to_existing_custom_location(self):
        custom = self.run_dir / 'custom-existing-install'
        self.assertEqual(self.install(custom), 0)
        sentinel = custom / 'user-library-sentinel.txt'
        sentinel.write_text('keep user data', encoding='utf-8')
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.registered_directory(), custom)
        self.assertFalse((self.default_dir / 'payload.txt').exists(), 'Upgrade created a second installation')
        self.assertEqual(sentinel.read_text(encoding='utf-8'), 'keep user data')

    def assert_running_image_blocks_replacement(self, image_name):
        target = self.run_dir / 'occupied-install'
        self.assertEqual(self.install(target), 0)
        marker = target / 'payload.txt'
        marker.write_text('old application payload', encoding='utf-8')
        image = target / image_name
        shutil.copyfile(Path(os.environ['SystemRoot']) / 'System32/cmd.exe', image)
        process = subprocess.Popen([str(image), '/d', '/q'], stdin=subprocess.PIPE,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.assertIsNone(process.poll(), 'Fixture process must be alive')
            result = self.install(target)
            self.assertNotEqual(result, 0, 'Installer accepted a running application image')
            self.assertIsNone(process.poll(), 'Installer closed a running application')
            self.assertEqual(marker.read_text(encoding='utf-8'), 'old application payload')
        finally:
            process.communicate(('exit' + os.linesep).encode(), timeout=10)
        self.assertEqual(self.install(target), 0, 'Retry after clean exit must succeed')
        self.assertEqual(marker.read_text(encoding='utf-8'), 'new application payload')

    def test_running_application_blocks_replacement(self):
        self.assert_running_image_blocks_replacement('HarkenSearch.exe')

    def test_running_updater_blocks_replacement(self):
        self.assert_running_image_blocks_replacement('HarkenSearchBootstrap.exe')

    def test_running_bridge_blocks_replacement(self):
        self.assert_running_image_blocks_replacement('HarkenSearchProToolsBridge.exe')

    def test_runtime_pair_updater_blocks_replacement(self):
        class FileIdInfo(ctypes.Structure):
            _fields_ = [('volume_serial', ctypes.c_uint64),
                        ('file_id', ctypes.c_ubyte * 16)]

        target = self.run_dir / 'occupied-install'
        self.assertEqual(self.install(target), 0)
        marker = target / 'payload.txt'
        marker.write_text('old application payload', encoding='utf-8')
        runtime_pair = self.run_dir / 'transaction' / 'runtime-pair'
        runtime_pair.mkdir(parents=True)
        image = runtime_pair / 'HarkenSearchBootstrap.exe'
        shutil.copyfile(Path(os.environ['SystemRoot']) / 'System32/cmd.exe', image)

        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32,
                                       ctypes.c_uint32, ctypes.c_void_p,
                                       ctypes.c_uint32, ctypes.c_uint32,
                                       ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.GetFileInformationByHandleEx.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                                        ctypes.c_void_p, ctypes.c_uint32]
        kernel.GetFileInformationByHandleEx.restype = ctypes.c_int
        kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
        kernel.CreateMutexW.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.WaitForSingleObject.restype = ctypes.c_uint32
        kernel.ReleaseMutex.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]

        directory = kernel.CreateFileW(str(target), 0x80, 7, None, 3, 0x02000000, None)
        self.assertNotEqual(directory, ctypes.c_void_p(-1).value)
        try:
            info = FileIdInfo()
            self.assertTrue(kernel.GetFileInformationByHandleEx(
                directory, 18, ctypes.byref(info), ctypes.sizeof(info)))
        finally:
            kernel.CloseHandle(directory)
        name = ('Global\\HarkenSearchInstallMutation_' +
                f'{info.volume_serial:016X}' +
                ''.join(f'{value:02X}' for value in info.file_id))
        mutex = kernel.CreateMutexW(None, False, name)
        self.assertTrue(mutex)
        self.assertIn(kernel.WaitForSingleObject(mutex, 0), (0, 0x80))
        process = subprocess.Popen([str(image), '/d', '/q'], stdin=subprocess.PIPE,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            self.assertIsNone(process.poll(), 'Runtime-pair updater copy must be alive')
            self.assertNotEqual(self.install(target), 0,
                                'Installer accepted an active update for the same directory')
            self.assertIsNone(process.poll(), 'Installer closed the updater copy')
            self.assertEqual(marker.read_text(encoding='utf-8'), 'old application payload')
        finally:
            process.communicate(('exit' + os.linesep).encode(), timeout=10)
            kernel.ReleaseMutex(mutex)
            kernel.CloseHandle(mutex)
        self.assertEqual(self.install(target), 0, 'Retry after update exit must succeed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--artifact-root', type=Path, default=ARTIFACT_ROOT)
    parser.add_argument('--iscc', type=Path, default=ISCC)
    arguments, remaining = parser.parse_known_args()
    ARTIFACT_ROOT = arguments.artifact_root.resolve()
    ISCC = arguments.iscc.resolve()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
