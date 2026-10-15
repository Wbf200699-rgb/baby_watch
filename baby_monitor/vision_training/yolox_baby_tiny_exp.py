from __future__ import annotations

import os
from pathlib import Path

from yolox.exp import Exp as YoloXBaseExp


class Exp(YoloXBaseExp):
    """YOLOX-Tiny 224 experiment matching the Renesas RA8P1 model contract."""

    def __init__(self):
        super().__init__()
        default_data = Path(__file__).resolve().parent / "data" / "processed" / "scene_coco"
        default_output = Path(__file__).resolve().parent / "artifacts" / "scene"

        self.num_classes = 5
        self.depth = 0.33
        self.width = 0.375
        self.input_size = (224, 224)
        self.test_size = (224, 224)
        self.multiscale_range = 1
        self.mosaic_scale = (0.80, 1.20)
        self.degrees = 5.0
        self.translate = 0.05
        self.shear = 1.0
        self.enable_mixup = False
        self.mosaic_prob = 0.8
        self.flip_prob = 0.5
        self.hsv_prob = 0.7

        self.data_dir = os.environ.get("BABY_VISION_COCO_ROOT", str(default_data))
        self.train_ann = "instances_train2017.json"
        self.val_ann = os.environ.get("BABY_VISION_VAL_ANN", "instances_val2017.json")
        self.val_image_dir = os.environ.get("BABY_VISION_VAL_IMAGE_DIR", "val2017")
        self.test_ann = "instances_test2017.json"
        self.data_num_workers = int(os.environ.get("BABY_VISION_WORKERS", "4"))

        self.max_epoch = int(os.environ.get("BABY_VISION_MAX_EPOCH", "120"))
        self.warmup_epochs = int(os.environ.get("BABY_VISION_WARMUP_EPOCHS", "5"))
        self.no_aug_epochs = int(os.environ.get("BABY_VISION_NO_AUG_EPOCHS", "15"))
        self.basic_lr_per_img = float(
            os.environ.get("BABY_VISION_BASIC_LR_PER_IMG", str(0.01 / 64.0))
        )
        self.eval_interval = 5
        self.print_interval = 20
        self.test_conf = 0.01
        self.nmsthre = 0.45
        self.output_dir = os.environ.get("BABY_VISION_OUTPUT_DIR", str(default_output))
        self.exp_name = os.environ.get(
            "BABY_VISION_EXP_NAME",
            "baby_vision_yolox_tiny_224_base",
        )
        self.seed = 20260725

    def get_eval_dataset(self, **kwargs):
        """Select annotations and image directory independently for holdout tests."""
        from yolox.data import COCODataset, ValTransform

        testdev = kwargs.get("testdev", False)
        legacy = kwargs.get("legacy", False)
        return COCODataset(
            data_dir=self.data_dir,
            json_file=self.test_ann if testdev else self.val_ann,
            name="test2017" if testdev else self.val_image_dir,
            img_size=self.test_size,
            preproc=ValTransform(legacy=legacy),
        )

    def eval(self, evaluator, model, is_distributed, half=False, return_outputs=False):
        """Use the portable pycocotools evaluator on Windows.

        Upstream YOLOX exposes its fast evaluator even when the optional C++
        extension is not built.  The failure then happens only at the first
        validation epoch and aborts an otherwise valid training run.  Standard
        COCOeval is slower but deterministic and requires no Visual Studio
        toolchain, which is the appropriate default for this workstation.
        """

        if os.environ.get("BABY_VISION_FAST_COCOEVAL", "0") != "1":
            import yolox.layers
            from pycocotools.cocoeval import COCOeval

            yolox.layers.COCOeval_opt = COCOeval
        return super().eval(
            evaluator,
            model,
            is_distributed,
            half,
            return_outputs=return_outputs,
        )
