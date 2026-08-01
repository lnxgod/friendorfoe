package com.friendorfoe.data.badge

import java.nio.file.Files
import java.nio.file.Path
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeCommandStartWiringTest {

    @Test
    fun everyMutationPlatformCallIsInsideTheSharedFinalGate() {
        val source = badgeRepositorySource()
        val usbBody = source.functionBody("writeUsbCommand")
        val usbReaderBody = source.functionBody("startReader")
        val readOnlyUsbWriterBody = source.functionBody("writeUsbLine")
        val httpBody = source.functionBody("postJsonCommand")
        val bleBody = source.functionBody("writeBleCommand")

        val mutationBodies = listOf(usbBody, httpBody, bleBody)
        assertEquals(
            "Adding a Badge mutation start requires extending this wiring contract",
            3,
            source.countOccurrences(FINAL_GATE_CALL),
        )
        assertEquals(
            3,
            mutationBodies.sumOf { it.countOccurrences(FINAL_GATE_CALL) },
        )

        assertPlatformCallIsInsideStartLambda(
            repositorySource = source,
            functionBody = usbBody,
            platformCall = ".bulkTransfer(",
            expectedCallCount = 1,
            repositoryExclusive = false,
        )
        assertEquals(3, source.countOccurrences(".bulkTransfer("))
        assertEquals(1, usbReaderBody.countOccurrences(".bulkTransfer("))
        assertEquals(1, readOnlyUsbWriterBody.countOccurrences(".bulkTransfer("))
        assertPlatformCallIsInsideStartLambda(
            repositorySource = source,
            functionBody = httpBody,
            platformCall = ".enqueue(",
            expectedCallCount = 1,
        )
        assertPlatformCallIsInsideStartLambda(
            repositorySource = source,
            functionBody = bleBody,
            platformCall = ".writeCharacteristic(",
            expectedCallCount = 2,
        )
    }

    @Test
    fun executePathsCannotBypassTheNamedAuthorizedMutationWriters() {
        val source = badgeRepositorySource()
        val usbExecution = source.functionBody("executeUsbOnce")
        val bleExecution = source.functionBody("executeBleOnce")
        val apExecution = source.functionBody("executeApHttpOnce")
        val debugExecution = source.functionBody("executeDebugBridgeOnce")
        val usbWriter = source.functionBody("writeUsbCommand")

        assertEquals(1, usbExecution.countOccurrences("writeUsbCommand("))
        assertFalse(usbExecution.contains("writeUsbLine("))
        assertEquals(1, bleExecution.countOccurrences("writeBleCommand("))
        assertFalse(bleExecution.contains("writeCharacteristic("))
        assertEquals(1, apExecution.countOccurrences("postJsonCommand("))
        assertEquals(1, debugExecution.countOccurrences("postJsonCommand("))

        assertEquals(2, source.countOccurrences("writeUsbCommand("))
        assertEquals(2, source.countOccurrences("writeBleCommand("))
        assertEquals(3, source.countOccurrences("postJsonCommand("))
        assertEquals(1, source.countOccurrences("FOF_CTL:"))
        assertEquals(1, usbWriter.countOccurrences("FOF_CTL:"))
    }

    private fun assertPlatformCallIsInsideStartLambda(
        repositorySource: String,
        functionBody: String,
        platformCall: String,
        expectedCallCount: Int,
        repositoryExclusive: Boolean = true,
    ) {
        assertEquals(1, functionBody.countOccurrences(FINAL_GATE_CALL))
        val gateCall = functionBody.balancedCall(FINAL_GATE_CALL)
        val startLambda = gateCall.namedLambdaBody("start")
        assertEquals(expectedCallCount, startLambda.countOccurrences(platformCall))
        assertEquals(
            "The platform mutation call must not escape its final-authority start lambda",
            startLambda.countOccurrences(platformCall),
            functionBody.countOccurrences(platformCall),
        )
        if (repositoryExclusive) {
            assertEquals(
                "The platform mutation call must not appear outside its named mutation function",
                functionBody.countOccurrences(platformCall),
                repositorySource.countOccurrences(platformCall),
            )
        }
    }

    private fun badgeRepositorySource(): String {
        val relativeCandidates = listOf(
            Path.of("app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt"),
            Path.of("android/app/src/main/java/com/friendorfoe/data/badge/BadgeUsbRepository.kt"),
        )
        val startingDirectory = Path.of(System.getProperty("user.dir")).toAbsolutePath()
        val repositoryFile = generateSequence(startingDirectory) { it.parent }
            .flatMap { root -> relativeCandidates.asSequence().map(root::resolve) }
            .firstOrNull(Files::isRegularFile)
            ?: error("Could not locate BadgeUsbRepository.kt from $startingDirectory")
        return Files.readAllBytes(repositoryFile).toString(Charsets.UTF_8)
    }

    private fun String.functionBody(functionName: String): String {
        val signature = Regex("""\bfun\s+${Regex.escape(functionName)}\s*\(""")
            .find(this)
            ?: error("Missing function $functionName")
        val openingParenthesis = signature.range.last
        val parameters = balancedRegion(openingParenthesis, '(', ')')
        val openingBrace = indexOf('{', openingParenthesis + parameters.length)
        check(openingBrace >= 0) { "Missing body for $functionName" }
        return balancedRegion(openingBrace, '{', '}')
    }

    private fun String.balancedCall(callName: String): String {
        val callStart = indexOf(callName)
        check(callStart >= 0) { "Missing call $callName" }
        val openingParenthesis = indexOf('(', callStart + callName.length)
        check(openingParenthesis >= 0) { "Missing arguments for $callName" }
        return balancedRegion(openingParenthesis, '(', ')')
    }

    private fun String.namedLambdaBody(argumentName: String): String {
        val match = Regex("""\b${Regex.escape(argumentName)}\s*=\s*\{""")
            .find(this)
            ?: error("Missing $argumentName lambda")
        return balancedRegion(match.range.last, '{', '}')
    }

    private fun String.balancedRegion(
        openingIndex: Int,
        openingCharacter: Char,
        closingCharacter: Char,
    ): String {
        check(getOrNull(openingIndex) == openingCharacter)
        var depth = 0
        var index = openingIndex
        var state = LexicalState.CODE
        var blockCommentDepth = 0
        while (index < length) {
            val current = this[index]
            val next = getOrNull(index + 1)
            when (state) {
                LexicalState.CODE -> when {
                    current == '/' && next == '/' -> {
                        state = LexicalState.LINE_COMMENT
                        index += 1
                    }
                    current == '/' && next == '*' -> {
                        state = LexicalState.BLOCK_COMMENT
                        blockCommentDepth = 1
                        index += 1
                    }
                    current == '"' -> state = LexicalState.STRING
                    current == '\'' -> state = LexicalState.CHARACTER
                    current == openingCharacter -> depth += 1
                    current == closingCharacter -> {
                        depth -= 1
                        if (depth == 0) return substring(openingIndex, index + 1)
                    }
                }
                LexicalState.STRING -> when {
                    current == '\\' -> index += 1
                    current == '"' -> state = LexicalState.CODE
                }
                LexicalState.CHARACTER -> when {
                    current == '\\' -> index += 1
                    current == '\'' -> state = LexicalState.CODE
                }
                LexicalState.LINE_COMMENT -> if (current == '\n') {
                    state = LexicalState.CODE
                }
                LexicalState.BLOCK_COMMENT -> when {
                    current == '/' && next == '*' -> {
                        blockCommentDepth += 1
                        index += 1
                    }
                    current == '*' && next == '/' -> {
                        blockCommentDepth -= 1
                        index += 1
                        if (blockCommentDepth == 0) state = LexicalState.CODE
                    }
                }
            }
            index += 1
        }
        error("Unbalanced $openingCharacter$closingCharacter region")
    }

    private fun String.countOccurrences(needle: String): Int {
        var count = 0
        var searchFrom = 0
        while (true) {
            val match = indexOf(needle, searchFrom)
            if (match < 0) return count
            count += 1
            searchFrom = match + needle.length
        }
    }

    private enum class LexicalState {
        CODE,
        STRING,
        CHARACTER,
        LINE_COMMENT,
        BLOCK_COMMENT,
    }

    private companion object {
        const val FINAL_GATE_CALL = "commandStarts.startIfAuthorized"
    }
}
