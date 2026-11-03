import hashlib,json,tempfile,unittest,subprocess,sys
from pathlib import Path
from qa_day_palette_editor import validate_package

class PackageGuardTests(unittest.TestCase):
    def test_wrong_executable_or_root_is_rejected_before_launch(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw);exe=root/'HarkenSearch.exe';exe.write_bytes(('isolated fixture '+root.as_posix()+chr(0)).encode())
            receipt=root/'receipt.json'
            value=dict(exePath=str(exe),exeSha256=hashlib.sha256(exe.read_bytes()).hexdigest(),isolationRoot=str(root))
            receipt.write_text(json.dumps(value),encoding='utf-8')
            validate_package(exe,root,receipt)
            with self.assertRaises(AssertionError):validate_package(exe,root/'wrong',receipt)
            value['isolationRoot']=str(root/'wrong')
            receipt.write_text(json.dumps(value),encoding='utf-8')
            with self.assertRaises(AssertionError):validate_package(exe,root/'wrong',receipt)
            value['isolationRoot']=str(root)
            receipt.write_text(json.dumps(value),encoding='utf-8')
            exe.write_bytes(b'wrong executable')
            with self.assertRaises(AssertionError):validate_package(exe,root,receipt)

    def test_optimized_python_is_rejected_before_any_package_access(self):
        result=subprocess.run([sys.executable,'-O','-c','from qa_day_palette_editor import validate_package;validate_package(None,None,None)'],cwd=Path(__file__).parent,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Optimized Python is unsupported',result.stderr)

if __name__=='__main__':unittest.main()
