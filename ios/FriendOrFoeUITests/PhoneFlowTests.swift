import XCTest

final class PhoneFlowTests: XCTestCase {
    private func launch(welcome: Bool = false, appearance: String = "System", largeText: Bool = false) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments = ["-completedWelcome", welcome ? "NO" : "YES", "-appearance", appearance]
        if largeText { app.launchArguments += ["-UIPreferredContentSizeCategoryName", "UICTContentSizeCategoryAccessibilityXXXL"] }
        app.launch()
        return app
    }

    private func capture(_ name: String) {
        let attachment = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    func testWelcomeOpensRecorderFirstAndRunsOfflineExample() {
        let app = launch(welcome: true, appearance: "Light")
        let begin = app.buttons["openPrivacy"]
        XCTAssertTrue(begin.waitForExistence(timeout: 10))
        capture("Welcome light")
        begin.tap()
        XCTAssertTrue(app.buttons["privacy.scan"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.buttons["Recorders"].isSelected)
        capture("Privacy light")
        app.buttons["privacy.guide"].tap()
        let match = app.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS[c] 'matches the Plaud recorder family'")).firstMatch
        XCTAssertTrue(match.waitForExistence(timeout: 5))
        capture("Offline recorder example")
        app.buttons["Done"].tap()
        app.buttons["Beacons"].tap()
        XCTAssertTrue(app.buttons["Add a known beacon UUID"].exists)
    }

    func testWelcomePrimaryActionAtLargestTextSize() {
        let app = launch(welcome: true, largeText: true)
        let button = app.buttons["openPrivacy"]
        XCTAssertTrue(button.waitForExistence(timeout: 10))
        XCTAssertTrue(button.isHittable)
        button.tap()
        XCTAssertTrue(app.navigationBars["Privacy"].waitForExistence(timeout: 5))
    }

    func testPermissionFreeTabsAndOfflineGuide() {
        let app = launch(appearance: "Dark")
        let tabs = app.tabBars.buttons
        tabs["Nearby"].tap()
        XCTAssertTrue(app.buttons["sky.start"].exists)
        tabs["Map"].tap()
        XCTAssertTrue(app.buttons["Browse Apple Maps without location"].exists)
        tabs["Station"].tap()
        XCTAssertTrue(app.buttons["Add server"].exists)
        app.buttons["Add server"].tap()
        XCTAssertTrue(app.navigationBars["Scanner server"].exists)
        app.buttons["Cancel"].tap()
        tabs["More"].tap()
        app.buttons["Aircraft & drone guide"].tap()
        let field = app.searchFields.firstMatch
        XCTAssertTrue(field.waitForExistence(timeout: 5))
        field.tap()
        field.typeText("B738")
        let result = app.buttons.matching(NSPredicate(format: "label CONTAINS[c] 'Boeing 737 NG'")).firstMatch
        XCTAssertTrue(result.waitForExistence(timeout: 4))
        tabs["Privacy"].tap()
        capture("Privacy dark")
    }
}
