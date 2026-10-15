from __future__ import annotations

import tensorflow as tf


def _head(x: tf.Tensor, classes: int) -> tf.Tensor:
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    return tf.keras.layers.Dense(classes, activation="softmax")(x)


def _ds_block(x: tf.Tensor, channels: int, stride: int = 1) -> tf.Tensor:
    x = tf.keras.layers.DepthwiseConv2D(3, strides=stride, padding="same", use_bias=False)(x)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU(max_value=6.0)(x)
    x = tf.keras.layers.Conv2D(channels, 1, padding="same", use_bias=False)(x)
    x = tf.keras.layers.BatchNormalization()(x)
    return tf.keras.layers.ReLU(max_value=6.0)(x)


def _build_cry_audio(shape: tuple[int, ...], classes: int, name: str) -> tf.keras.Model:
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Conv2D(32, 3, strides=(2, 1), padding="same", use_bias=False)(inputs)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU(max_value=6.0)(x)
    for channels, stride in ((32, 1), (48, 2), (64, 2), (64, 1)):
        x = _ds_block(x, channels, stride)
    return tf.keras.Model(inputs, _head(x, classes), name=name)


def build_cry(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    """Legacy 49x40 cry model kept for compatibility with earlier artifacts."""
    return _build_cry_audio(shape, classes, "cry_ds_cnn")


def build_cry_detection(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    return _build_cry_audio(shape, classes, "cry_detection_ds_cnn")


def build_cry_detection_tiny(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    """Small dense model with a deterministic INT8 CPU fallback for first board tests."""
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Flatten(name="flatten")(inputs)
    x = tf.keras.layers.Dense(16, activation="relu", name="hidden")(x)
    outputs = tf.keras.layers.Dense(classes, activation="softmax", name="probabilities")(x)
    return tf.keras.Model(inputs, outputs, name="cry_detection_tiny_mlp")


def build_cry_detection_mlp(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    """Compact two-layer fallback that is straightforward to run without TFLM."""
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Flatten(name="flatten")(inputs)
    x = tf.keras.layers.Dense(64, activation="relu", name="hidden_64")(x)
    x = tf.keras.layers.Dense(16, activation="relu", name="hidden_16")(x)
    outputs = tf.keras.layers.Dense(classes, activation="softmax", name="probabilities")(x)
    return tf.keras.Model(inputs, outputs, name="cry_detection_mlp")


def build_cry_type(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    return _build_cry_audio(shape, classes, "cry_type_ds_cnn")


def build_cry_type_mlp(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    """Compact reason classifier sharing the deployed MFCC implementation."""
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Flatten(name="flatten")(inputs)
    x = tf.keras.layers.Dense(64, activation="relu", name="hidden_64")(x)
    x = tf.keras.layers.Dense(16, activation="relu", name="hidden_16")(x)
    outputs = tf.keras.layers.Dense(classes, activation="softmax", name="probabilities")(x)
    return tf.keras.Model(inputs, outputs, name="cry_type_mlp")


def build_cry_type_v1_mlp(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    return build_cry_type_mlp(shape, classes)


def build_cry_type_sequence_mlp(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    return build_cry_type_mlp(shape, classes)


def build_pressure(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPool2D()(x)
    x = tf.keras.layers.Conv2D(24, 3, padding="same", activation="relu")(x)
    x = tf.keras.layers.MaxPool2D()(x)
    x = tf.keras.layers.Conv2D(32, 3, padding="same", activation="relu")(x)
    return tf.keras.Model(inputs, _head(x, classes), name="pressure_tiny_cnn")


def build_thermal(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Conv2D(16, 3, strides=2, padding="same", use_bias=False)(inputs)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU(max_value=6.0)(x)
    for channels, stride in ((24, 2), (32, 2), (48, 2), (64, 1)):
        x = _ds_block(x, channels, stride)
    return tf.keras.Model(inputs, _head(x, classes), name="thermal_ds_cnn")


def build_vision(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    base = tf.keras.applications.MobileNetV3Small(
        input_shape=shape, alpha=0.5, include_top=False, weights=None,
        minimalistic=True, pooling="avg"
    )
    outputs = tf.keras.layers.Dense(classes, activation="softmax")(base.output)
    return tf.keras.Model(base.input, outputs, name="vision_mobilenet_v3_small_050")


def build_fusion(shape: tuple[int, ...], classes: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape)
    x = tf.keras.layers.Dense(32, activation="relu")(inputs)
    x = tf.keras.layers.Dense(24, activation="relu")(x)
    return tf.keras.Model(inputs, tf.keras.layers.Dense(classes, activation="softmax")(x), name="fusion_mlp")


BUILDERS = {
    "cry": build_cry,
    "cry_detection": build_cry_detection,
    "cry_detection_mlp": build_cry_detection_mlp,
    "cry_detection_tiny": build_cry_detection_tiny,
    "cry_type": build_cry_type,
    "cry_type_mlp": build_cry_type_mlp,
    "cry_type_v1_mlp": build_cry_type_v1_mlp,
    "cry_type_sequence_mlp": build_cry_type_sequence_mlp,
    "pressure": build_pressure,
    "thermal": build_thermal,
    "vision": build_vision,
    "fusion": build_fusion,
}
