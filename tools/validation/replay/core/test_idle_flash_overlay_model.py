import unittest

from idle_flash_overlay_model import FlashBandCost, IdleFlashOverlay


class IdleFlashOverlayModelTest(unittest.TestCase):
    def test_cost_is_flash_to_existing_internal_buffer_not_psram(self):
        cost = FlashBandCost(480, 48, 4)
        cost.validate()
        self.assertEqual(cost.bytes_per_frame, 46080)
        self.assertEqual((cost.psram_read_bytes, cost.psram_write_bytes), (0, 0))

    def test_budgeted_cluster_finishes_on_base_then_rests(self):
        overlay = IdleFlashOverlay(4, 240, 5300)
        events = [overlay.tick(idle=True, budget_ok=True) for _ in range(4)]
        self.assertEqual([item[1] for item in events], [0, 1, 2, 3])
        self.assertEqual(events[-1], ("done", 3, 5300))

    def test_busy_before_start_drops_without_showing_any_frame(self):
        overlay = IdleFlashOverlay(4, 240, 5300)
        self.assertEqual(overlay.tick(idle=True, budget_ok=False), ("drop", None, 5300))

    def test_busy_mid_cluster_aborts_directly_to_base_without_replay(self):
        overlay = IdleFlashOverlay(4, 240, 5300)
        self.assertEqual(overlay.tick(idle=True, budget_ok=True), ("show", 0, 240))
        self.assertEqual(overlay.tick(idle=True, budget_ok=False), ("abort_to_base", 3, 5300))
        self.assertEqual(overlay.step, 0)


if __name__ == "__main__":
    unittest.main()
