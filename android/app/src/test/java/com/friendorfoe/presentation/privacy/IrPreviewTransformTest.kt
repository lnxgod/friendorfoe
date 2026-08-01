package com.friendorfoe.presentation.privacy

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class IrPreviewTransformTest {
    @Test
    fun appliesCropRotationAndFrontCameraMirror() {
        val metadata = AnalysisFrameMetadata(
            imageWidth = 1920,
            imageHeight = 1080,
            crop = IntRect(240, 0, 1680, 1080),
            rotationDegrees = 90,
            frontCamera = true,
        )

        val point = transformAnalysisPoint(
            source = FloatPoint(240f, 0f),
            metadata = metadata,
            previewWidth = 1080f,
            previewHeight = 1440f,
        )

        assertEquals(0f, point.x, 0.5f)
        assertEquals(0f, point.y, 0.5f)
    }

    @Test
    fun centerRemainsCenterForAllQuarterTurns() {
        listOf(0, 90, 180, 270).forEach { rotation ->
            val point = transformAnalysisPoint(
                source = FloatPoint(500f, 250f),
                metadata = AnalysisFrameMetadata(
                    imageWidth = 1000,
                    imageHeight = 500,
                    crop = IntRect(0, 0, 1000, 500),
                    rotationDegrees = rotation,
                    frontCamera = false,
                ),
                previewWidth = 400f,
                previewHeight = 800f,
            )

            assertEquals(200f, point.x, 0.5f)
            assertEquals(400f, point.y, 0.5f)
        }
    }

    @Test
    fun quarterTurnsMapCroppedTopLeftToExpectedCorners() {
        val expected = mapOf(
            0 to FloatPoint(0f, 0f),
            90 to FloatPoint(500f, 0f),
            180 to FloatPoint(1000f, 500f),
            270 to FloatPoint(0f, 1000f),
        )

        expected.forEach { (rotation, wanted) ->
            val point = transformAnalysisPoint(
                source = FloatPoint(100f, 50f),
                metadata = AnalysisFrameMetadata(
                    imageWidth = 1200,
                    imageHeight = 700,
                    crop = IntRect(100, 50, 1100, 550),
                    rotationDegrees = rotation,
                    frontCamera = false,
                ),
                previewWidth = if (rotation % 180 == 0) 1000f else 500f,
                previewHeight = if (rotation % 180 == 0) 500f else 1000f,
            )

            assertEquals(wanted.x, point.x, 0.5f)
            assertEquals(wanted.y, point.y, 0.5f)
        }
    }

    @Test
    fun frontCameraMirrorsAfterRotation() {
        val back = transformAnalysisPoint(
            source = FloatPoint(250f, 100f),
            metadata = AnalysisFrameMetadata(
                imageWidth = 1000,
                imageHeight = 500,
                crop = IntRect(0, 0, 1000, 500),
                rotationDegrees = 0,
                frontCamera = false,
            ),
            previewWidth = 1000f,
            previewHeight = 500f,
        )
        val front = transformAnalysisPoint(
            source = FloatPoint(250f, 100f),
            metadata = AnalysisFrameMetadata(
                imageWidth = 1000,
                imageHeight = 500,
                crop = IntRect(0, 0, 1000, 500),
                rotationDegrees = 0,
                frontCamera = true,
            ),
            previewWidth = 1000f,
            previewHeight = 500f,
        )

        assertEquals(250f, back.x, 0.5f)
        assertEquals(750f, front.x, 0.5f)
        assertEquals(back.y, front.y, 0.5f)
    }

    @Test
    fun fillCenterCropsOnlyTheOverflowingAxis() {
        val point = transformAnalysisPoint(
            source = FloatPoint(0f, 0f),
            metadata = AnalysisFrameMetadata(
                imageWidth = 1000,
                imageHeight = 500,
                crop = IntRect(0, 0, 1000, 500),
                rotationDegrees = 0,
                frontCamera = false,
            ),
            previewWidth = 400f,
            previewHeight = 400f,
        )

        assertEquals(-200f, point.x, 0.5f)
        assertEquals(0f, point.y, 0.5f)
    }

    @Test
    fun clampsPointsOutsideAnalysisCropBeforeTransforming() {
        val beforeCrop = transformAnalysisPoint(
            source = FloatPoint(-500f, -500f),
            metadata = AnalysisFrameMetadata(
                imageWidth = 1000,
                imageHeight = 500,
                crop = IntRect(100, 50, 900, 450),
                rotationDegrees = 0,
                frontCamera = false,
            ),
            previewWidth = 800f,
            previewHeight = 400f,
        )
        val afterCrop = transformAnalysisPoint(
            source = FloatPoint(5_000f, 5_000f),
            metadata = AnalysisFrameMetadata(
                imageWidth = 1000,
                imageHeight = 500,
                crop = IntRect(100, 50, 900, 450),
                rotationDegrees = 0,
                frontCamera = false,
            ),
            previewWidth = 800f,
            previewHeight = 400f,
        )

        assertEquals(FloatPoint(0f, 0f), beforeCrop)
        assertEquals(FloatPoint(800f, 400f), afterCrop)
    }

    @Test
    fun rejectsNonQuarterTurnRotation() {
        val metadata = AnalysisFrameMetadata(
            imageWidth = 100,
            imageHeight = 100,
            crop = IntRect(0, 0, 100, 100),
            rotationDegrees = 45,
            frontCamera = false,
        )

        assertThrows(IllegalArgumentException::class.java) {
            transformAnalysisPoint(FloatPoint(50f, 50f), metadata, 100f, 100f)
        }
    }
}
