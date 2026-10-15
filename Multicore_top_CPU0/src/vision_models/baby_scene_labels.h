/*
 * Deployment class order.  This order must match vision_models.yaml.
 */
#ifndef BABY_SCENE_LABELS_H
#define BABY_SCENE_LABELS_H

#define BABY_SCENE_CLASS_COUNT (5U)

static const char * const g_baby_scene_labels[BABY_SCENE_CLASS_COUNT] =
{
    "infant",
    "head_face",
    "blanket",
    "soft_object",
    "caregiver"
};

#endif /* BABY_SCENE_LABELS_H */
