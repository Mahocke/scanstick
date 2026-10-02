import importlib.util
import os
import tempfile
import unittest


MODULE = os.path.join(os.path.dirname(__file__), "..", "empfaenger", "scan-receiver.py")


class ReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        os.environ["SCAN_INBOX"] = cls.tmp.name
        spec = importlib.util.spec_from_file_location("scan_receiver", MODULE)
        cls.receiver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.receiver)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_partial_body_is_rejected(self):
        self.assertIn("only 3 of 4", self.receiver.vollstaendig("scan.bin", b"abc", 4))

    def test_pdf_without_end_marker_is_rejected(self):
        self.assertIn("without %%EOF", self.receiver.vollstaendig("scan.pdf", b"%PDF", 4))

    def test_complete_pdf_is_accepted(self):
        data = b"%PDF-1.4\n%%EOF\n"
        self.assertIsNone(self.receiver.vollstaendig("scan.pdf", data, len(data)))

    def test_store_never_overwrites_an_existing_scan(self):
        first = self.receiver.ablegen(self.tmp.name, "scan.pdf", b"first")
        second = self.receiver.ablegen(self.tmp.name, "scan.pdf", b"second")
        self.assertNotEqual(first, second)
        with open(first, "rb") as f:
            self.assertEqual(f.read(), b"first")
        with open(second, "rb") as f:
            self.assertEqual(f.read(), b"second")


if __name__ == "__main__":
    unittest.main()
