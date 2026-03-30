package com.friendorfoe.presentation.about

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.friendorfoe.BuildConfig

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AboutScreen(
    onBack: () -> Unit,
    viewModel: AboutViewModel? = null
) {
    val context = LocalContext.current

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("About") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Navigate back"
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Header
            item {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = "Friend or Foe",
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "Identify aircraft and drones in the sky using augmented reality",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                )
            }

            // How It Works
            item {
                SectionCard(title = "How It Works") {
                    BulletItem("Point your phone at the sky")
                    BulletItem("Aircraft detected via ADS-B radio data (adsb.fi, airplanes.live, OpenSky)")
                    BulletItem("Drones detected via Bluetooth Remote ID and WiFi")
                    BulletItem("Camera detects visual objects and correlates with radio data")
                    BulletItem("Aircraft classified by type: commercial, military, helicopter, cargo, emergency, and more")
                }
            }

            // Detection Sources
            item {
                SectionCard(title = "Detection Sources") {
                    SourceBadge(
                        color = Color(0xFF4CAF50),
                        letter = "A",
                        label = "ADS-B aircraft (transponder data)"
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    SourceBadge(
                        color = Color(0xFF9C27B0),
                        letter = "B",
                        label = "Bluetooth Remote ID drones"
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    SourceBadge(
                        color = Color(0xFF2196F3),
                        letter = "W",
                        label = "WiFi-detected drones"
                    )
                }
            }

            // Category Colors
            item {
                SectionCard(title = "Category Colors") {
                    CategoryLegendRow(Color(0xFF4CAF50), "Commercial", "Airline flights", null)
                    CategoryLegendRow(Color(0xFFFFA726), "General Aviation", "Private / light aircraft", null)
                    CategoryLegendRow(Color(0xFFF44336), "Military", "Military aircraft", "MIL")
                    CategoryLegendRow(Color(0xFF26A69A), "Helicopter", "Rotorcraft", "HELI")
                    CategoryLegendRow(Color(0xFFE65100), "Government", "Government / law enforcement", "GOV")
                    CategoryLegendRow(Color(0xFFE91E63), "Emergency", "Squawk 7500/7600/7700", "EMG")
                    CategoryLegendRow(Color(0xFF8D6E63), "Cargo", "Cargo carriers", "CGO")
                    CategoryLegendRow(Color(0xFF2196F3), "Drone", "UAS", null)
                    CategoryLegendRow(Color(0xFF616161), "Ground Vehicle", "ADS-B surface vehicles", "GND")
                    CategoryLegendRow(Color(0xFF9E9E9E), "Unknown", "Unidentified", null)
                }
            }

            // Tips
            item {
                SectionCard(title = "Tips") {
                    BulletItem("Best outdoors with clear sky view")
                    BulletItem("Calibrate compass by moving phone in figure-8")
                    BulletItem("Works offline for drone detection (no internet needed)")
                    BulletItem("ADS-B requires internet for aircraft data")
                }
            }

            // Contact / Feedback
            item {
                SectionCard(title = "Contact & Feedback") {
                    Text(
                        text = "Report bugs or request features:",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = "lnxgod@gmail.com",
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.primary,
                        textDecoration = TextDecoration.Underline,
                        modifier = Modifier.clickable {
                            val intent = Intent(Intent.ACTION_SENDTO).apply {
                                data = Uri.parse("mailto:lnxgod@gmail.com")
                                putExtra(Intent.EXTRA_SUBJECT, "Friend or Foe Feedback")
                            }
                            context.startActivity(intent)
                        }
                    )
                }
            }

            // Permissions Explained
            item {
                SectionCard(title = "Permissions") {
                    PermissionRow("Camera", "AR viewfinder and visual detection")
                    PermissionRow("Location", "Calculate bearing and distance to aircraft")
                    PermissionRow("Bluetooth", "Detect drones via Remote ID")
                    PermissionRow("WiFi", "Detect drones via WiFi signals")
                }
            }

            // Legal / Data
            item {
                SectionCard(title = "Privacy & Data") {
                    BulletItem("Aircraft data from public ADS-B feeds")
                    BulletItem("No personal data is collected or transmitted")
                    BulletItem("All detection data stays on your device")
                }
            }

            // Detection Settings
            if (viewModel != null) {
                item {
                    SectionCard(title = "Sensor Backend (ESP32 Network)") {
                        SettingsToggle(
                            title = "Backend-Only Mode",
                            description = "Use ESP32 sensor network only — disables local phone detection for lower battery use",
                            initialValue = viewModel.backendOnlyMode,
                            onToggle = { viewModel.setBackendOnlyMode(it) }
                        )
                        SettingsToggle(
                            title = "Sensor Backend Connection",
                            description = "Connect to ESP32 sensors for Remote ID, WiFi drone detection, and triangulation",
                            initialValue = viewModel.sensorBackendEnabled,
                            onToggle = { viewModel.setSensorBackendEnabled(it) }
                        )

                        // Backend URL + Test Connection
                        var urlText by remember { mutableStateOf(viewModel.backendUrl) }
                        val connectionStatus by viewModel.connectionStatus.collectAsStateWithLifecycle()
                        Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                            Text(
                                "Backend URL",
                                style = MaterialTheme.typography.bodyMedium,
                                fontWeight = FontWeight.SemiBold
                            )
                            Spacer(modifier = Modifier.height(4.dp))
                            OutlinedTextField(
                                value = urlText,
                                onValueChange = { urlText = it },
                                modifier = Modifier.fillMaxWidth(),
                                textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                                singleLine = true,
                                placeholder = { Text("http://192.168.x.x:8000/") }
                            )
                            Spacer(modifier = Modifier.height(4.dp))
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                TextButton(onClick = {
                                    viewModel.setBackendUrl(urlText)
                                }) { Text("Save URL") }
                                TextButton(onClick = {
                                    viewModel.setBackendUrl(urlText)
                                    viewModel.testConnection()
                                }) { Text("Test Connection") }
                            }
                            connectionStatus?.let { status ->
                                val isOk = status.startsWith("Connected")
                                Text(
                                    text = status,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = if (isOk) Color(0xFF4CAF50) else Color(0xFFF44336),
                                    modifier = Modifier.padding(top = 4.dp)
                                )
                            }
                        }
                    }
                }
                item {
                    // Local detection sources — grayed out description when backend-only
                    val backendOnly = viewModel.backendOnlyMode
                    SectionCard(title = if (backendOnly) "Local Detection (Disabled — Backend-Only Mode)" else "Local Detection Sources") {
                        SettingsToggle(
                            title = "ADS-B Aircraft",
                            description = "Commercial flights, GA, military via transponder data",
                            initialValue = viewModel.adsbEnabled,
                            onToggle = { viewModel.setAdsbEnabled(it) }
                        )
                        SettingsToggle(
                            title = "BLE Remote ID (Drones)",
                            description = "FAA-compliant drone detection via Bluetooth LE",
                            initialValue = viewModel.bleRidEnabled,
                            onToggle = { viewModel.setBleRidEnabled(it) }
                        )
                        SettingsToggle(
                            title = "WiFi Detection (Drones)",
                            description = "Drone SSID patterns, DJI DroneID, WiFi beacons",
                            initialValue = viewModel.wifiEnabled,
                            onToggle = { viewModel.setWifiEnabled(it) }
                        )
                    }
                }
                item {
                    SectionCard(title = "Privacy Detection") {
                        SettingsToggle(
                            title = "Privacy Scanner",
                            description = "Smart glasses, trackers, hidden cameras, body cams, attack tools, Tesla Sentry via BLE + WiFi",
                            initialValue = viewModel.privacyEnabled,
                            onToggle = { viewModel.setPrivacyEnabled(it) }
                        )
                        SettingsToggle(
                            title = "Stalker / Follower Detection",
                            description = "Alert when BLE devices follow you across locations or linger nearby",
                            initialValue = viewModel.stalkerEnabled,
                            onToggle = { viewModel.setStalkerEnabled(it) }
                        )
                        SettingsToggle(
                            title = "WiFi Evil Twin Detection",
                            description = "Detect rogue access points, evil twin attacks, and WiFi Pineapple karma attacks",
                            initialValue = viewModel.wifiAnomalyEnabled,
                            onToggle = { viewModel.setWifiAnomalyEnabled(it) }
                        )
                        SettingsToggle(
                            title = "Ultrasonic Beacon Detection",
                            description = "Detect inaudible 18-22 kHz tracking beacons from ads/stores (requires microphone)",
                            initialValue = viewModel.ultrasonicEnabled,
                            onToggle = { viewModel.setUltrasonicEnabled(it) }
                        )
                    }
                }
                item {
                    SectionCard(title = "Sweep Tools") {
                        BulletItem("EMF Sweep — Use magnetometer to find hidden electronics at close range")
                        BulletItem("IR Camera Scan — Use front camera to detect night-vision IR LEDs in dark rooms")
                        BulletItem("BLE Direction Finder — Rotate 360° to locate a detected BLE device")
                    }
                }
            }

            // Version
            item {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 16.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Text(
                        text = "Version ${BuildConfig.VERSION_NAME}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
                    )
                    Text(
                        text = "Build ${BuildConfig.VERSION_CODE}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.4f)
                    )
                }
                Spacer(modifier = Modifier.height(16.dp))
            }
        }
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(8.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Spacer(modifier = Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
private fun BulletItem(text: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp)
    ) {
        Text(
            text = "\u2022",
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.width(16.dp)
        )
        Text(
            text = text,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
private fun SourceBadge(color: Color, letter: String, label: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(28.dp)
                .background(color, RoundedCornerShape(6.dp)),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = letter,
                color = Color.White,
                fontWeight = FontWeight.Bold,
                fontSize = 14.sp
            )
        }
        Spacer(modifier = Modifier.width(12.dp))
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
private fun CategoryLegendRow(color: Color, name: String, description: String, badge: String?) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Color dot
        Box(
            modifier = Modifier
                .size(14.dp)
                .background(color, RoundedCornerShape(3.dp))
        )
        Spacer(modifier = Modifier.width(10.dp))
        // Name + optional badge
        Text(
            text = name,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.width(110.dp)
        )
        if (badge != null) {
            Text(
                text = badge,
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Bold,
                color = Color.White,
                modifier = Modifier
                    .background(color, RoundedCornerShape(4.dp))
                    .padding(horizontal = 4.dp, vertical = 1.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
        }
        // Description
        Text(
            text = description,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
        )
    }
}

@Composable
private fun PermissionRow(permission: String, reason: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = permission,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.weight(0.3f)
        )
        Text(
            text = reason,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
            modifier = Modifier.weight(0.7f),
            textAlign = TextAlign.End
        )
    }
}

@Composable
private fun SettingsToggle(
    title: String,
    description: String,
    initialValue: Boolean,
    onToggle: (Boolean) -> Unit
) {
    var enabled by remember { mutableStateOf(initialValue) }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium
            )
            Text(
                text = description,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        Spacer(modifier = Modifier.width(8.dp))
        Switch(
            checked = enabled,
            onCheckedChange = {
                enabled = it
                onToggle(it)
            }
        )
    }
}
