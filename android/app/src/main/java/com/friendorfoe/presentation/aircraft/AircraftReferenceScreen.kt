package com.friendorfoe.presentation.aircraft

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.clickable
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.platform.testTag
import com.friendorfoe.presentation.filter.FilterSearchField
import com.friendorfoe.presentation.reference.ReferenceNoMatches
import com.friendorfoe.presentation.reference.ReferenceExpansionHint
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
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
import androidx.compose.material.icons.automirrored.filled.ArrowBack
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
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.components.ReferenceImage
import com.friendorfoe.presentation.util.*
import com.friendorfoe.presentation.util.AircraftCategory
import com.friendorfoe.presentation.util.AircraftDatabase
import com.friendorfoe.presentation.util.AircraftReference

/**
 * Aircraft reference guide screen showing all known aircraft types with photos,
 * specs, and descriptions. Filterable by category and searchable by name.
 *
 * Follows the same UI patterns as DroneReferenceScreen for consistency.
 *
 * @param onBack Callback to navigate back
 * @param initialTypeFilter Optional ICAO type code to pre-filter and highlight
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AircraftReferenceScreen(
    onBack: () -> Unit,
    initialTypeFilter: String? = null
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Aircraft guide") },
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
        Column(modifier = Modifier.padding(innerPadding)) {
            AircraftReferenceContent(initialTypeFilter = initialTypeFilter)
        }
    }
}

/**
 * Aircraft reference content without Scaffold wrapper.
 * Used by both the standalone screen and the tabbed ReferenceGuideScreen.
 */
@Composable
fun AircraftReferenceContent(
    initialTypeFilter: String? = null
) {
    var searchQuery by rememberSaveable {
        mutableStateOf(
            if (initialTypeFilter != null) {
                AircraftDatabase.matchByTypeCode(initialTypeFilter)?.name ?: initialTypeFilter
            } else ""
        )
    }
    var selectedCategory by rememberSaveable { mutableStateOf<AircraftCategory?>(null) }
    var expandedAircraftId by rememberSaveable { mutableStateOf<String?>(null) }

    val filteredAircraft = remember(searchQuery, selectedCategory) {
        var aircraft = if (searchQuery.isNotBlank()) {
            AircraftDatabase.search(searchQuery)
        } else {
            AircraftDatabase.allAircraft
        }
        if (selectedCategory != null) {
            aircraft = aircraft.filter { it.category == selectedCategory }
        }
        aircraft
    }

    Column(modifier = Modifier.fillMaxSize()) {
        FilterSearchField(
            query = searchQuery,
            onQueryChange = { searchQuery = it },
            placeholder = "Search aircraft",
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
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
            items(AircraftCategory.entries.toList()) { category ->
                FilterChip(
                    selected = selectedCategory == category,
                    onClick = {
                        selectedCategory = if (selectedCategory == category) null else category
                    },
                    label = { Text(category.label) },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = MaterialTheme.colorScheme.primaryContainer,
                        selectedLabelColor = MaterialTheme.colorScheme.onPrimaryContainer
                    )
                )
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        // Results count
        Text(
            text = "${filteredAircraft.size} aircraft",
            modifier = Modifier.padding(horizontal = 16.dp),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Spacer(modifier = Modifier.height(4.dp))

        if (filteredAircraft.isEmpty()) {
            ReferenceNoMatches("aircraft", onReset = { searchQuery = ""; selectedCategory = null })
        }

        if (filteredAircraft.isNotEmpty()) LazyColumn(
            modifier = Modifier.fillMaxSize().testTag("aircraft_reference_results"),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            items(filteredAircraft, key = { it.id }) { aircraft ->
                AircraftReferenceCard(
                    aircraft = aircraft,
                    isExpanded = expandedAircraftId == aircraft.id,
                    onToggle = {
                        expandedAircraftId = if (expandedAircraftId == aircraft.id) null else aircraft.id
                    }
                )
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AircraftReferenceCard(
    aircraft: AircraftReference,
    isExpanded: Boolean,
    onToggle: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .animateContentSize()
            .testTag("reference_card_${aircraft.id}")
            .semantics { stateDescription = if (isExpanded) "Expanded" else "Collapsed" }
            .clickable(role = Role.Button, onClickLabel = if (isExpanded) "Hide details" else "View details", onClick = onToggle),
        shape = RoundedCornerShape(12.dp),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        )
    ) {
        Column {
            // Photo
            ReferenceImage(
                model = aircraftReferencePhotoUrl(aircraft),
                description = aircraft.name,
                silhouetteRes = silhouetteDrawableRes(silhouetteForAircraftReference(aircraft.category)),
                caption = referenceImageCaption(aircraft.photoAsset),
                modifier = Modifier
                    .fillMaxWidth()
                    .height(if (isExpanded) 220.dp else 170.dp)
                    .clip(RoundedCornerShape(topStart = 12.dp, topEnd = 12.dp)),
            )

            Column(modifier = Modifier.padding(12.dp)) {
                Text(
                    text = aircraft.name,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                )
                FlowRow(
                    modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Text(
                        text = aircraft.manufacturer,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    AircraftCategoryBadge(aircraft.category)
                }

                if (isExpanded) {
                    Spacer(modifier = Modifier.height(8.dp))

                    // Description
                    Text(
                        text = aircraft.description,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f)
                    )

                    Spacer(modifier = Modifier.height(8.dp))

                    // Specs
                    Card(
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surfaceVariant
                        ),
                        shape = RoundedCornerShape(8.dp)
                    ) {
                        Text(
                            text = aircraft.specs,
                            modifier = Modifier.padding(8.dp),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }

                    // ICAO type codes
                    if (aircraft.icaoTypeCodes.isNotEmpty()) {
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            text = "ICAO: ${aircraft.icaoTypeCodes.joinToString(", ")}",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                ReferenceExpansionHint(isExpanded)
            }
        }
    }
}

@Composable
private fun AircraftCategoryBadge(category: AircraftCategory) {
    val color = aircraftCategoryColor(category)
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

private fun aircraftCategoryColor(category: AircraftCategory): Color {
    return when (category) {
        AircraftCategory.NARROWBODY -> Color(0xFF2196F3)    // blue
        AircraftCategory.WIDEBODY -> Color(0xFF1565C0)      // dark blue
        AircraftCategory.REGIONAL -> Color(0xFF00BCD4)      // cyan
        AircraftCategory.TURBOPROP -> Color(0xFF009688)     // teal
        AircraftCategory.BIZJET -> Color(0xFF9C27B0)        // purple
        AircraftCategory.HELICOPTER -> Color(0xFFFF9800)    // orange
        AircraftCategory.FIGHTER -> Color(0xFFF44336)       // red
        AircraftCategory.CARGO -> Color(0xFF795548)         // brown
        AircraftCategory.LIGHTPLANE -> Color(0xFF4CAF50)    // green
        AircraftCategory.TRAINER -> Color(0xFF607D8B)       // blue grey
    }
}
