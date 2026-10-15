package com.example.baby_watch.ui.esp32cam_img

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.baby_watch.data.repository.CamStatus
import com.example.baby_watch.data.repository.Esp32CamManager

@Composable
fun Esp32CamImage(
    host: String,
    modifier: Modifier = Modifier,
) {
    val state by Esp32CamManager.state.collectAsState()

    LaunchedEffect(host, state.status) {
        when (state.status) {
            CamStatus.Idle,
            CamStatus.Error -> Esp32CamManager.connect(host)
            else -> Unit
        }
    }

    Box(modifier = modifier.clip(RoundedCornerShape(18.dp)).background(Color(0xFF10201C))) {
        val frame = state.frame
        if (frame != null) {
            Image(
                bitmap = frame.asImageBitmap(),
                contentDescription = "实时画面",
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop
            )
        } else {
            EmptyCameraState(
                text = when (state.status) {
                    CamStatus.Idle -> "摄像头未连接"
                    CamStatus.Connecting -> "正在连接摄像头..."
                    CamStatus.Error -> state.errorMessage ?: "视频连接中断"
                    CamStatus.Connected -> "等待画面"
                }
            )
        }
    }
}

@Composable
private fun EmptyCameraState(text: String) {
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(Icons.Default.Videocam, contentDescription = null, tint = Color.White.copy(alpha = 0.34f), modifier = Modifier.size(48.dp))
            Spacer(modifier = Modifier.height(10.dp))
            Text(text, fontSize = 13.sp, fontWeight = FontWeight.Medium, color = Color.White.copy(alpha = 0.72f))
        }
    }
}
