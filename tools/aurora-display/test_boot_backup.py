# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import tempfile
import tomllib
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("boot_backup", Path(__file__).with_name("boot_backup.py"))
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class BootBackupTest(unittest.TestCase):
    def test_archive_covers_early_boot_and_kernel_without_changing_source(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            boot = root / "boot"
            (boot / "efi/m1n1").mkdir(parents=True)
            (boot / "grub").mkdir()
            (boot / "vmlinuz-linux-aurora").write_bytes(b"kernel\0data")
            (boot / "initramfs-linux-aurora.img").write_bytes(b"initramfs")
            (boot / "efi/m1n1/boot.bin").write_bytes(b"m1n1+U-Boot+DTB")
            (boot / "kernel link").symlink_to("vmlinuz-linux-aurora")
            (boot / "grub/grub.cfg").write_text("menuentry 'Aurora' {\n linux /vmlinuz-linux-aurora quiet\n initrd /initramfs-linux-aurora.img\n}\n")
            before = helper.digest(boot / "efi/m1n1/boot.bin")
            archive, count = helper.snapshot(boot, root / "backup")
            receipt = tomllib.loads((archive.parent / "manifest.toml").read_text())
            self.assertTrue(receipt["verified_archive"])
            self.assertFalse(receipt["rollback_tested"])
            self.assertEqual(count, 5)
            self.assertEqual(helper.digest(archive), receipt["archive_sha256"])
            self.assertEqual(before, helper.digest(boot / "efi/m1n1/boot.bin"))
            self.assertEqual(helper.inventory(boot)["menu_titles"], ["Aurora"])
            self.assertEqual(helper.inventory(boot)["boot_bin_paths"], ["efi/m1n1/boot.bin"])
            self.assertEqual(archive.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(ValueError):
                helper.snapshot(boot, root / "backup")

    def test_empty_tree_cannot_be_reported_as_verified_backup(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "boot").mkdir()
            with self.assertRaises(ValueError):
                helper.snapshot(root / "boot", root / "backup")


if __name__ == "__main__":
    unittest.main()
