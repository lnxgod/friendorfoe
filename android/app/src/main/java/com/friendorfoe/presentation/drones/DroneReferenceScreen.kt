package com.friendorfoe.presentation.drones

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import com.friendorfoe.presentation.util.AutonomyLevel
import com.friendorfoe.presentation.util.DroneCategory
import com.friendorfoe.presentation.util.DroneDatabase
import com.friendorfoe.presentation.util.DroneReference
import com.friendorfoe.presentation.util.RiskLevel
import com.friendorfoe.presentation.util.ThreatClassification

/**
 * Drone reference guide screen showing all known drone types with photos,
 * specs, and descriptions. Filterable by category and searchable by name.
 *
 * @param onBack Callback to navigate back
 * @param initialManufacturerFilter Optional manufacturer to pre-filter results
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DroneReferenceScreen(
    onBack: () -> Unit,
    initialManufacturerFilter: String? = null
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Drone Reference Guide") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Text(
                            text = "\u2190",
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.onSurface
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            )
        }
    ) { innerPadding ->
        Column(modifier = Modifier.padding(innerPadding)) {
            DroneReferenceContent(initialManufacturerFilter = initialManufacturerFilter)
        }
    }
}

/**
 * Drone reference content without Scaffold wrapper.
 * Used by both the standalone screen and the tabbed ReferenceGuideScreen.
 */
@Composable
fun DroneReferenceContent(
    initialManufacturerFilter: String? = null
) {
    var searchQuery by remember { mutableStateOf(initialManufacturerFilter ?: "") }
    var selectedCategory by remember { mutableStateOf<DroneCategory?>(null) }
    var expandedDroneId by remember { mutableStateOf<String?>(null) }

    val filteredDrones = remember(searchQuery, selectedCategory) {
        var drones = if (searchQuery.isNotBlank()) {
            DroneDatabase.search(searchQuery)
        } else {
            DroneDatabase.allDrones
        }
        if (selectedCategory != null) {
            drones = drones.filter { it.category == selectedCategory }
        }
        drones
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Search bar
        TextField(
            value = searchQuery,
            onValueChange = { searchQuery = it },
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            placeholder = { Text("Search drones...") },
            leadingIcon = {
                Icon(Icons.Default.Search, contentDescription = "Search")
            },
            singleLine = true,
            shape = RoundedCornerShape(12.dp),
            colors = TextFieldDefaults.colors(
                focusedIndicatorColor = Color.Transparent,
                unfocusedIndicatorColor = Color.Transparent
            )
        )

        // Category filter chips
        LazyRow(
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            item {
                FilterChip(
                    selected = selectedCategory == null,
                    onClick = { selectedCategory = null },
                    label = { Text("All") },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = MaterialTheme.colorScheme.primary,
                        selectedLabelColor = MaterialTheme.colorScheme.onPrimary
                    )
                )
            }
            items(DroneCategory.entries.toList()) { category ->
                FilterChip(
                    selected = selectedCategory == category,
                    onClick = {
                        selectedCategory = if (selectedCategory == category) null else category
                    },
                    label = { Text(category.label) },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = categoryChipColor(category),
                        selectedLabelColor = Color.White
                    )
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // Results count
        Text(
            text = "${filteredDrones.size} drone${if (filteredDrones.size != 1) "s" else ""}",
            modifier = Modifier.padding(horizontal = 16.dp),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
        )

        Spacer(modifier = Modifier.height(4.dp))

        // Drone list
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            items(filteredDrones, key = { it.id }) { drone ->
                DroneReferenceCard(
                    drone = drone,
                    isExpanded = expandedDroneId == drone.id,
                    onToggle = {
                        expandedDroneId = if (expandedDroneId == drone.id) null else drone.id
                    }
                )
            }
        }
    }
}

@Composable
private fun DroneReferenceCard(
    drone: DroneReference,
    isExpanded: Boolean,
    onToggle: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onToggle),
        shape = RoundedCornerShape(12.dp),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        )
    ) {
        Column {
            // Photo
            AsyncImage(
                model = "file:///android_asset/${drone.photoAsset}",
                contentDescription = drone.name,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(if (isExpanded) 200.dp else 140.dp)
                    .clip(RoundedCornerShape(topStart = 12.dp, topEnd = 12.dp)),
                contentScale = ContentScale.Crop
            )

            Column(modifier = Modifier.padding(12.dp)) {
                // Name and category badge
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = drone.name,
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Text(
                            text = drone.manufacturer,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
                        )
                    }
                    CategoryBadge(drone.category)
                }

                // Risk level badge, country, sanctioned warning
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    drone.riskLevel?.let { risk ->
                        RiskBadge(risk)
                    }
                    drone.countryOfOrigin?.let { country ->
                        Text(
                            text = "${countryFlag(country)} $country",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)
                        )
                    }
                    if (drone.sanctionedManufacturer) {
                        Card(
                            colors = CardDefaults.cardColors(
                                containerColor = Color(0xFFF44336).copy(alpha = 0.15f)
                            ),
                            shape = RoundedCornerShape(16.dp)
                        ) {
                            Text(
                                text = "SANCTIONED",
                                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = FontWeight.Bold,
                                color = Color(0xFFF44336)
                            )
                        }
                    }
                }

                if (isExpanded) {
                    Spacer(modifier = Modifier.height(8.dp))

                    // Description
                    Text(
                        text = drone.description,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f)
                    )

                    Spacer(modifier = Modifier.height(8.dp))

                    // Threat classification, autonomy, swarm
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        drone.threatClassification?.let { tc ->
                            FilterChip(
                                selected = false,
                                onClick = {},
                                label = { Text(tc.label, style = MaterialTheme.typography.labelSmall) },
                                colors = FilterChipDefaults.filterChipColors(
                                    containerColor = MaterialTheme.colorScheme.surfaceVariant
                                )
                            )
                        }
                        drone.autonomyLevel?.let { al ->
                            FilterChip(
                                selected = false,
                                onClick = {},
                                label = { Text(al.label, style = MaterialTheme.typography.labelSmall) },
                                colors = FilterChipDefaults.filterChipColors(
                                    containerColor = MaterialTheme.colorScheme.surfaceVariant
                                )
                            )
                        }
                        if (drone.swarmCapable) {
                            FilterChip(
                                selected = true,
                                onClick = {},
                                label = { Text("Swarm", style = MaterialTheme.typography.labelSmall) },
                                colors = FilterChipDefaults.filterChipColors(
                                    selectedContainerColor = Color(0xFFF44336).copy(alpha = 0.15f),
                                    selectedLabelColor = Color(0xFFF44336)
                                )
                            )
                        }
                    }

                    // Operational history
                    drone.operationalHistory?.let { history ->
                        Spacer(modifier = Modifier.height(4.dp))
                        Card(
                            colors = CardDefaults.cardColors(
                                containerColor = Color(0xFFF44336).copy(alpha = 0.05f)
                            ),
                            shape = RoundedCornerShape(8.dp)
                        ) {
                            Column(modifier = Modifier.padding(8.dp)) {
                                Text(
                                    text = "Operational History",
                                    style = MaterialTheme.typography.labelSmall,
                                    fontWeight = FontWeight.Bold,
                                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                                )
                                Spacer(modifier = Modifier.height(4.dp))
                                Text(
                                    text = history,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                                )
                            }
                        }
                    }

                    Spacer(modifier = Modifier.height(8.dp))

                    // Specs
                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surfaceVariant
                        ),
                        shape = RoundedCornerShape(8.dp)
                    ) {
                        Text(
                            text = drone.specs,
                            modifier = Modifier.padding(8.dp),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }

                    // WiFi patterns (if any)
                    if (drone.wifiPatterns.isNotEmpty()) {
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            text = "WiFi patterns: ${drone.wifiPatterns.joinToString(", ")}",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.4f)
                        )
                    }
                } else {
                    // Collapsed: show specs summary
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = drone.specs,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }
    }
}

@Composable
private fun CategoryBadge(category: DroneCategory) {
    val color = categoryChipColor(category)
    Card(
        colors = CardDefaults.cardColors(
            containerColor = color.copy(alpha = 0.15f)
        ),
        shape = RoundedCornerShape(16.dp)
    ) {
        Text(
            text = category.label,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
            color = color
        )
    }
}

@Composable
private fun RiskBadge(riskLevel: RiskLevel) {
    val color = Color(riskLevel.color)
    Card(
        colors = CardDefaults.cardColors(
            containerColor = color.copy(alpha = 0.15f)
        ),
        shape = RoundedCornerShape(16.dp)
    ) {
        Text(
            text = riskLevel.label,
            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.Bold,
            color = color
        )
    }
}

private fun countryFlag(country: String): String {
    return when (country.lowercase()) {
        "usa" -> "\uD83C\uDDFA\uD83C\uDDF8"
        "china" -> "\uD83C\uDDE8\uD83C\uDDF3"
        "iran" -> "\uD83C\uDDEE\uD83C\uDDF7"
        "russia" -> "\uD83C\uDDF7\uD83C\uDDFA"
        "turkey" -> "\uD83C\uDDF9\uD83C\uDDF7"
        "france" -> "\uD83C\uDDEB\uD83C\uDDF7"
        "israel" -> "\uD83C\uDDEE\uD83C\uDDF1"
        "poland" -> "\uD83C\uDDF5\uD83C\uDDF1"
        "australia" -> "\uD83C\uDDE6\uD83C\uDDFA"
        "ukraine" -> "\uD83C\uDDFA\uD83C\uDDE6"
        "germany" -> "\uD83C\uDDE9\uD83C\uDDEA"
        "switzerland" -> "\uD83C\uDDE8\uD83C\uDDED"
        else -> "\uD83C\uDFF3\uFE0F"
    }
}

private fun categoryChipColor(category: DroneCategory): Color {
    return when (category) {
        DroneCategory.CONSUMER -> Color(0xFF2196F3)
        DroneCategory.ENTERPRISE -> Color(0xFF009688)
        DroneCategory.RACING_FPV -> Color(0xFFFF9800)
        DroneCategory.MILITARY_RECON -> Color(0xFF607D8B)
        DroneCategory.MILITARY_STRIKE -> Color(0xFFF44336)
        DroneCategory.LOITERING_MUNITION -> Color(0xFF9C27B0)
        DroneCategory.FPV_COMBAT -> Color(0xFFB71C1C)
    }
}
