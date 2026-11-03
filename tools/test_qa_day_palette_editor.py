import hashlib,json,tempfile,unittest
from pathlib import Path
from qa_day_palette_editor import validate_package

class PackageGuardTests(unittest.TestCase):
    def test_wrong_executable_or_root_is_rejected_before_launch(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw);exe=root/'HarkenSearch.exe';exe.write_bytes(b'isolated fixture')
            receipt=root/'receipt.json'
            value=dict(exePath=str(exe),exeSha256=hashlib.sha256(exe.read_bytes()).hexdigest(),isolationRoot=str(root))
            receipt.write_text(json.dumps(value),encoding='utf-8')
            validate_package(exe,root,receipt)
            with self.assertRaises(AssertionError):validate_package(exe,root/'wrong',receipt)
            exe.write_bytes(b'wrong executable')
            with self.assertRaises(AssertionError):validate_package(exe,root,receipt)

if __name__=='__main__':unittest.main()
