#pragma once

/**
 * Friend or Foe -- Bayesian Sensor Fusion Engine
 *
 * Multi-sensor evidence combination using log-odds representation.
 * Ported from Android BayesianFusionEngine.kt.
 *
 * Key behaviors:
 * - Agreeing sources boost confidence: WiFi(0.3) + RemoteID(0.9) -> fused 0.97
 * - Evidence decays over time (half-life = 30 seconds)
 * - Log-odds representation for efficient sequential updates
 * - Clamped to prevent overflow/underflow
 */

#include "detection_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the fusion engine. Must be called before any other fusion calls.
 */
void bayesian_fusion_init(void);

/**
 * Update the belief state for a candidate with new sensor evidence.
 *
 * @param candidate_id  Unique string ID for the candidate detection
 * @param source        Detection source (DETECTION_SRC_*)
 * @param sensor_confidence  The sensor's own confidence (0.0-1.0)
 * @param now_ms        Current timestamp in milliseconds
 * @return Updated fused probability (0.0-1.0)
 */
float bayesian_fusion_update(const char *candidate_id, uint8_t source,
                             float sensor_confidence, int64_t now_ms);

/**
 * Get the current fused probability for a candidate.
 * Applies time decay based on elapsed time since last update.
 *
 * @param candidate_id  Unique string ID for the candidate
 * @param now_ms        Current timestamp in milliseconds
 * @return Fused probability (0.0-1.0), or PRIOR_PROBABILITY if unknown
 */
float bayesian_fusion_get_probability(const char *candidate_id, int64_t now_ms);

/**
 * Prune stale candidates that haven't been updated within DETECTION_STALE_MS.
 * Should be called periodically (e.g., every PRUNE_INTERVAL_MS).
 *
 * @param now_ms  Current timestamp in milliseconds
 */
void bayesian_fusion_prune(int64_t now_ms);

/**
 * Get the number of active (in-use) tracked candidates.
 */
int bayesian_fusion_get_active_count(void);

/**
 * Reset all belief states. Clears the entire fusion table.
 */
void bayesian_fusion_reset(void);

#ifdef __cplusplus
}
#endif
