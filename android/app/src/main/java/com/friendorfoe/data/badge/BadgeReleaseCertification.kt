package com.friendorfoe.data.badge

data class BadgeReleaseCertification(
    val mutationsByTransport: Map<BadgeTransport, Set<BadgeCapability>> = emptyMap(),
) {
    fun forTransport(transport: BadgeTransport?): Set<BadgeCapability> =
        transport?.let { mutationsByTransport[it] }.orEmpty()
}

internal val CheckedInBadgeReleaseCertification = BadgeReleaseCertification()
