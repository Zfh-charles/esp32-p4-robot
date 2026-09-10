import unittest

from idle_life_cluster_model import IdleLifeCluster


def make_cluster() -> IdleLifeCluster:
    return IdleLifeCluster((1, 4, 7, 8), 240, 4200, 6900)


class IdleLifeClusterTest(unittest.TestCase):
    def test_closed_cluster_plays_keyframes_then_rests(self):
        cluster = make_cluster()
        shown = []
        for _ in range(4):
            shown.append(cluster.target())
            event, delay = cluster.tick(True)
        self.assertEqual([1, 4, 7, 8], shown)
        self.assertEqual("done", event)
        self.assertGreaterEqual(delay, 4200)
        self.assertLessEqual(delay, 6900)
        self.assertEqual(1, cluster.target())

    def test_busy_mid_cluster_aborts_to_base_without_replay(self):
        cluster = make_cluster()
        self.assertEqual(1, cluster.target())
        cluster.tick(True)
        self.assertEqual(4, cluster.target())
        event, delay = cluster.tick(False)
        self.assertEqual(("abort_repair", 240), (event, delay))
        self.assertEqual(8, cluster.target())
        event, delay = cluster.tick(True)
        self.assertEqual("repair_done", event)
        self.assertGreaterEqual(delay, 4200)
        self.assertEqual(1, cluster.target())

    def test_busy_before_first_frame_drops_whole_token(self):
        cluster = make_cluster()
        event, delay = cluster.tick(False)
        self.assertEqual("drop", event)
        self.assertFalse(cluster.repair_pending)
        self.assertGreaterEqual(delay, 4200)
        self.assertEqual(1, cluster.target())

    def test_repair_retries_never_resume_motion(self):
        cluster = make_cluster()
        cluster.tick(True)
        cluster.tick(False)
        for _ in range(3):
            self.assertEqual(8, cluster.target())
            cluster.tick(False)
        self.assertTrue(cluster.repair_pending)
        self.assertEqual(8, cluster.target())


if __name__ == "__main__":
    unittest.main()
