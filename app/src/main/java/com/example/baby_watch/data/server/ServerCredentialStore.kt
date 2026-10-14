package com.example.baby_watch.data.server

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.core.content.edit
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

internal class ServerCredentialStore(context: Context) {
    private val preferences = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    init {
        migrateLegacyCloudProfile()
    }

    fun activeMode(): ServerEndpointMode = when (
        preferences.getString(KEY_ACTIVE_MODE, null)?.lowercase()
    ) {
        MODE_LOCAL -> ServerEndpointMode.Local
        else -> ServerEndpointMode.Cloud
    }

    fun setActiveMode(mode: ServerEndpointMode) {
        preferences.edit { putString(KEY_ACTIVE_MODE, mode.preferenceValue) }
    }

    fun load(): ServerCredentials = load(activeMode())

    fun load(mode: ServerEndpointMode): ServerCredentials {
        val keys = keysFor(mode)
        val baseUrl = preferences.getString(keys.baseUrl, mode.defaultUrl)
            ?.let(::normalizeServerUrl)
            .orEmpty()
            .ifBlank { mode.defaultUrl }
        return ServerCredentials(
            baseUrl = baseUrl,
            deviceId = FIXED_DEVICE_ID,
            username = preferences.getString(keys.username, "").orEmpty(),
            password = decryptPassword(keys),
            mode = mode,
        )
    }

    fun view(): ServerConfigurationView = view(activeMode())

    /**
     * Returns both saved profiles while projecting [mode] through the legacy convenience fields.
     * [ServerConfigurationView.activeMode] always reports the persisted active profile.
     */
    fun view(mode: ServerEndpointMode): ServerConfigurationView {
        val cloudCredentials = load(ServerEndpointMode.Cloud)
        val localCredentials = load(ServerEndpointMode.Local)
        val selected = when (mode) {
            ServerEndpointMode.Cloud -> cloudCredentials
            ServerEndpointMode.Local -> localCredentials
        }
        return ServerConfigurationView(
            baseUrl = selected.baseUrl,
            deviceId = selected.deviceId,
            username = selected.username,
            hasPassword = selected.password.isNotBlank(),
            activeMode = activeMode(),
            cloud = cloudCredentials.toView(),
            local = localCredentials.toView(),
        )
    }

    fun save(baseUrl: String, username: String, newPassword: String?) {
        save(activeMode(), baseUrl, username, newPassword)
    }

    fun save(
        mode: ServerEndpointMode,
        baseUrl: String,
        username: String,
        newPassword: String?,
    ) {
        val normalizedUrl = normalizeServerUrl(baseUrl)
        require(isAllowedServerUrl(normalizedUrl, mode)) {
            "Server URL is not allowed for $mode mode"
        }
        val keys = keysFor(mode)
        val encrypted = newPassword?.takeIf(String::isNotEmpty)?.let(::encrypt)
        preferences.edit {
            putString(keys.baseUrl, normalizedUrl)
            putString(keys.username, username.trim())
            if (encrypted != null) {
                putString(keys.passwordCiphertext, encrypted.ciphertext)
                putString(keys.passwordIv, encrypted.iv)
            } else if (newPassword != null) {
                // null means "keep the saved password"; an explicit empty value clears it.
                remove(keys.passwordCiphertext)
                remove(keys.passwordIv)
            }
        }
    }

    fun lastHandledEventId(namespace: String): Long? {
        val key = eventCursorKey(namespace)
        if (preferences.contains(key)) return preferences.getLong(key, 0L)

        // Releases predating per-origin cursors stored one global value. It belongs to the old
        // configured cloud origin only; never copy it into a newly selected local Plan B profile.
        val legacyCloudNamespace = "${load(ServerEndpointMode.Cloud).baseUrl}|$FIXED_DEVICE_ID"
        if (namespace == legacyCloudNamespace && preferences.contains(KEY_LAST_HANDLED_EVENT_ID)) {
            val legacyValue = preferences.getLong(KEY_LAST_HANDLED_EVENT_ID, 0L)
            preferences.edit { putLong(key, legacyValue) }
            return legacyValue
        }
        return null
    }

    fun setLastHandledEventId(namespace: String, id: Long) {
        preferences.edit { putLong(eventCursorKey(namespace), id) }
    }

    private fun eventCursorKey(namespace: String): String =
        "${KEY_LAST_HANDLED_EVENT_ID}_${namespace.hashCode().toUInt().toString(16)}"

    private fun migrateLegacyCloudProfile() {
        val cloudKeys = keysFor(ServerEndpointMode.Cloud)
        preferences.edit {
            if (!preferences.contains(cloudKeys.baseUrl) && preferences.contains(KEY_BASE_URL)) {
                putString(cloudKeys.baseUrl, preferences.getString(KEY_BASE_URL, DEFAULT_SERVER_URL))
            }
            if (!preferences.contains(cloudKeys.username) && preferences.contains(KEY_USERNAME)) {
                putString(cloudKeys.username, preferences.getString(KEY_USERNAME, ""))
            }
            if (
                !preferences.contains(cloudKeys.passwordCiphertext) &&
                preferences.contains(KEY_PASSWORD_CIPHERTEXT)
            ) {
                putString(
                    cloudKeys.passwordCiphertext,
                    preferences.getString(KEY_PASSWORD_CIPHERTEXT, null),
                )
            }
            if (
                !preferences.contains(cloudKeys.passwordIv) &&
                preferences.contains(KEY_PASSWORD_IV)
            ) {
                putString(cloudKeys.passwordIv, preferences.getString(KEY_PASSWORD_IV, null))
            }
        }
    }

    private fun ServerCredentials.toView(): ServerEndpointView = ServerEndpointView(
        baseUrl = baseUrl,
        username = username,
        hasPassword = password.isNotBlank(),
    )

    private fun encrypt(plaintext: String): EncryptedValue {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey())
        val ciphertext = cipher.doFinal(plaintext.toByteArray(Charsets.UTF_8))
        return EncryptedValue(
            ciphertext = Base64.encodeToString(ciphertext, Base64.NO_WRAP),
            iv = Base64.encodeToString(cipher.iv, Base64.NO_WRAP),
        )
    }

    private fun decryptPassword(keys: ProfileKeys): String {
        val ciphertextText = preferences.getString(keys.passwordCiphertext, null) ?: return ""
        val ivText = preferences.getString(keys.passwordIv, null) ?: return ""
        return runCatching {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            val iv = Base64.decode(ivText, Base64.NO_WRAP)
            cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), GCMParameterSpec(128, iv))
            val plaintext = cipher.doFinal(Base64.decode(ciphertextText, Base64.NO_WRAP))
            plaintext.toString(Charsets.UTF_8)
        }.getOrElse { "" }
    }

    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(ANDROID_KEY_STORE).apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }

        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEY_STORE).run {
            init(
                KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                )
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setRandomizedEncryptionRequired(true)
                    .build()
            )
            generateKey()
        }
    }

    private data class ProfileKeys(
        val baseUrl: String,
        val username: String,
        val passwordCiphertext: String,
        val passwordIv: String,
    )

    private data class EncryptedValue(
        val ciphertext: String,
        val iv: String,
    )

    private val ServerEndpointMode.defaultUrl: String
        get() = when (this) {
            ServerEndpointMode.Cloud -> DEFAULT_SERVER_URL
            ServerEndpointMode.Local -> DEFAULT_LOCAL_SERVER_URL
        }

    private val ServerEndpointMode.preferenceValue: String
        get() = when (this) {
            ServerEndpointMode.Cloud -> MODE_CLOUD
            ServerEndpointMode.Local -> MODE_LOCAL
        }

    private fun keysFor(mode: ServerEndpointMode): ProfileKeys = when (mode) {
        ServerEndpointMode.Cloud -> ProfileKeys(
            baseUrl = KEY_CLOUD_BASE_URL,
            username = KEY_CLOUD_USERNAME,
            passwordCiphertext = KEY_CLOUD_PASSWORD_CIPHERTEXT,
            passwordIv = KEY_CLOUD_PASSWORD_IV,
        )
        ServerEndpointMode.Local -> ProfileKeys(
            baseUrl = KEY_LOCAL_BASE_URL,
            username = KEY_LOCAL_USERNAME,
            passwordCiphertext = KEY_LOCAL_PASSWORD_CIPHERTEXT,
            passwordIv = KEY_LOCAL_PASSWORD_IV,
        )
    }

    private companion object {
        const val PREFERENCES_NAME = "mobile_server_settings"
        const val KEY_ACTIVE_MODE = "active_endpoint_mode"
        const val KEY_CLOUD_BASE_URL = "cloud_base_url"
        const val KEY_CLOUD_USERNAME = "cloud_username"
        const val KEY_CLOUD_PASSWORD_CIPHERTEXT = "cloud_password_ciphertext"
        const val KEY_CLOUD_PASSWORD_IV = "cloud_password_iv"
        const val KEY_LOCAL_BASE_URL = "local_base_url"
        const val KEY_LOCAL_USERNAME = "local_username"
        const val KEY_LOCAL_PASSWORD_CIPHERTEXT = "local_password_ciphertext"
        const val KEY_LOCAL_PASSWORD_IV = "local_password_iv"
        const val MODE_CLOUD = "cloud"
        const val MODE_LOCAL = "local"

        // Legacy single-profile keys. Keep read compatibility across in-place upgrades.
        const val KEY_BASE_URL = "base_url"
        const val KEY_USERNAME = "username"
        const val KEY_PASSWORD_CIPHERTEXT = "password_ciphertext"
        const val KEY_PASSWORD_IV = "password_iv"
        const val KEY_LAST_HANDLED_EVENT_ID = "last_handled_event_id"
        const val KEY_ALIAS = "baby_watch_mobile_server_credentials"
        const val ANDROID_KEY_STORE = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
    }
}
