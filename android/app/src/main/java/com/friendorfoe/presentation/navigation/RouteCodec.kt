package com.friendorfoe.presentation.navigation

import java.net.URLEncoder
import java.nio.charset.StandardCharsets

fun encodeRouteSegment(value: String): String =
    URLEncoder.encode(value, StandardCharsets.UTF_8.name()).replace("+", "%20")
