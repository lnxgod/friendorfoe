package com.friendorfoe.presentation.util

/**
 * Built-in aircraft reference database for visual identification.
 *
 * Contains specs, descriptions, and photo asset references for major commercial,
 * military, and general aviation aircraft types. Used by the Aircraft Reference
 * Screen to help users identify aircraft, and linked from aircraft detail cards.
 *
 * Covers 160 aircraft types referencing bundled photo assets.
 */

data class AircraftReference(
    val id: String,
    val name: String,
    val manufacturer: String,
    val category: AircraftCategory,
    val description: String,
    val specs: String,
    val photoAsset: String,
    val icaoTypeCodes: List<String>
)

enum class AircraftCategory(val label: String) {
    NARROWBODY("Narrowbody"),
    WIDEBODY("Widebody"),
    REGIONAL("Regional Jet"),
    TURBOPROP("Turboprop"),
    BIZJET("Business Jet"),
    HELICOPTER("Helicopter"),
    FIGHTER("Fighter / Military"),
    CARGO("Cargo / Transport"),
    LIGHTPLANE("Light Aircraft"),
    TRAINER("Trainer")
}

object AircraftDatabase {

    val allAircraft: List<AircraftReference> = listOf(
        // ══════════════════════════════════════
        // ── Narrowbody Airliners ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "b737_classic",
            name = "Boeing 737 Classic",
            manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "Second-generation 737 variants (-300/-400/-500) produced 1984-2000. Recognizable by round engine nacelles. Many still in service worldwide with cargo operators.",
            specs = "Passengers: 128-168 | Range: 4,400km | Cruise: Mach 0.745 | Engines: 2x CFM56",
            photoAsset = "aircraft/B734.jpg",
            icaoTypeCodes = listOf("B734", "B735")
        ),
        AircraftReference(
            id = "b737_ng",
            name = "Boeing 737 NG",
            manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "Next-Generation 737 (-700/-800/-900) with blended winglets and flat-bottomed engine nacelles. The world's most delivered aircraft family. Backbone of low-cost carriers globally.",
            specs = "Passengers: 126-220 | Range: 5,765km | Cruise: Mach 0.785 | Engines: 2x CFM56-7B",
            photoAsset = "aircraft/B738.jpg",
            icaoTypeCodes = listOf("B737", "B738", "B739")
        ),
        AircraftReference(
            id = "b737_max",
            name = "Boeing 737 MAX",
            manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "Latest 737 variant with CFM LEAP-1B engines, split-tip winglets, and redesigned cabin. Grounded 2019-2020 after two fatal crashes. Now flying again worldwide.",
            specs = "Passengers: 138-230 | Range: 6,570km | Cruise: Mach 0.79 | Engines: 2x LEAP-1B",
            photoAsset = "aircraft/B38M.jpg",
            icaoTypeCodes = listOf("B38M", "B39M")
        ),
        AircraftReference(
            id = "a318",
            name = "Airbus A318",
            manufacturer = "Airbus",
            category = AircraftCategory.NARROWBODY,
            description = "Smallest A320 family member. Rare in service; most famously used by Air France for premium London City-JFK service using steep approach capability.",
            specs = "Passengers: 107-132 | Range: 5,750km | Cruise: Mach 0.78 | Engines: 2x CFM56/PW6000",
            photoAsset = "aircraft/A318.jpg",
            icaoTypeCodes = listOf("A318")
        ),
        AircraftReference(
            id = "a319",
            name = "Airbus A319",
            manufacturer = "Airbus",
            category = AircraftCategory.NARROWBODY,
            description = "Shortened A320 variant popular with airlines needing range over capacity. Common as VIP/government transport (A319CJ). Distinctive small sharklets optional.",
            specs = "Passengers: 124-156 | Range: 6,850km | Cruise: Mach 0.78 | Engines: 2x CFM56/V2500",
            photoAsset = "aircraft/A319.jpg",
            icaoTypeCodes = listOf("A319")
        ),
        AircraftReference(
            id = "a320",
            name = "Airbus A320",
            manufacturer = "Airbus",
            category = AircraftCategory.NARROWBODY,
            description = "The aircraft that established Airbus as a major player. First fly-by-wire narrowbody. Over 10,000 delivered. Competes directly with Boeing 737-800.",
            specs = "Passengers: 150-186 | Range: 6,100km | Cruise: Mach 0.78 | Engines: 2x CFM56/V2500",
            photoAsset = "aircraft/A320.jpg",
            icaoTypeCodes = listOf("A320")
        ),
        AircraftReference(
            id = "a321",
            name = "Airbus A321",
            manufacturer = "Airbus",
            category = AircraftCategory.NARROWBODY,
            description = "Stretched A320 and the bestselling single-aisle variant. The A321XLR extends range to 8,700km, enabling transatlantic narrowbody operations.",
            specs = "Passengers: 185-236 | Range: 5,950km | Cruise: Mach 0.78 | Engines: 2x CFM56/V2500",
            photoAsset = "aircraft/A321.jpg",
            icaoTypeCodes = listOf("A321")
        ),
        AircraftReference(
            id = "a320neo",
            name = "Airbus A320neo",
            manufacturer = "Airbus",
            category = AircraftCategory.NARROWBODY,
            description = "New Engine Option variant with LEAP-1A or PW1100G engines and sharklet wingtips. 20% fuel savings over CEO. Most-ordered commercial aircraft in history.",
            specs = "Passengers: 150-194 | Range: 6,300km | Cruise: Mach 0.78 | Engines: 2x LEAP-1A/PW1100G",
            photoAsset = "aircraft/A20N.jpg",
            icaoTypeCodes = listOf("A20N")
        ),
        AircraftReference(
            id = "a321neo",
            name = "Airbus A321neo",
            manufacturer = "Airbus",
            category = AircraftCategory.NARROWBODY,
            description = "Best-selling aircraft in Airbus history. The A321LR and XLR variants open long-haul routes previously requiring widebodies. Identifiable by large engine nacelles and sharklets.",
            specs = "Passengers: 180-244 | Range: 7,400km | Cruise: Mach 0.78 | Engines: 2x LEAP-1A/PW1100G",
            photoAsset = "aircraft/A21N.jpg",
            icaoTypeCodes = listOf("A21N")
        ),
        AircraftReference(
            id = "b757",
            name = "Boeing 757",
            manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "Narrow-body workhorse known for exceptional takeoff performance and range. Popular for transatlantic routes and high-altitude airports. Production ended 2004 but no true replacement exists.",
            specs = "Passengers: 200-295 | Range: 7,250km | Cruise: Mach 0.80 | Engines: 2x RB211/PW2000",
            photoAsset = "aircraft/B752.jpg",
            icaoTypeCodes = listOf("B752", "B753")
        ),
        AircraftReference(
            id = "b717",
            name = "Boeing 717",
            manufacturer = "Boeing (McDonnell Douglas)",
            category = AircraftCategory.NARROWBODY,
            description = "Originally the MD-95, last of the DC-9 lineage. Rear-mounted engines and T-tail. Operated primarily by Delta Air Lines and Hawaiian Airlines.",
            specs = "Passengers: 106-134 | Range: 2,645km | Cruise: Mach 0.77 | Engines: 2x BR715",
            photoAsset = "aircraft/B712.jpg",
            icaoTypeCodes = listOf("B712")
        ),
        AircraftReference(
            id = "a220",
            name = "Airbus A220",
            manufacturer = "Airbus (Bombardier)",
            category = AircraftCategory.NARROWBODY,
            description = "Originally the Bombardier C Series. Purpose-built 100-150 seat jet with advanced composites and PW1500G geared turbofan. Exceptional fuel efficiency and passenger comfort.",
            specs = "Passengers: 108-160 | Range: 6,300km | Cruise: Mach 0.78 | Engines: 2x PW1500G",
            photoAsset = "aircraft/A220.jpg",
            icaoTypeCodes = listOf("A220", "BCS1", "BCS3")
        ),
        AircraftReference(
            id = "md80",
            name = "McDonnell Douglas MD-80",
            manufacturer = "McDonnell Douglas",
            category = AircraftCategory.NARROWBODY,
            description = "Stretched DC-9 derivative with rear-mounted engines and T-tail. Once the backbone of American Airlines. Recognizable by its distinctive long fuselage and pencil-thin silhouette.",
            specs = "Passengers: 130-172 | Range: 3,800km | Cruise: Mach 0.76 | Engines: 2x JT8D-200",
            photoAsset = "aircraft/MD80.jpg",
            icaoTypeCodes = listOf("MD80")
        ),
        AircraftReference(
            id = "b707",
            name = "Boeing 707",
            manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "The jet that launched the Jet Age. First commercially successful jetliner. Military variants (C-137, E-3, E-6, KC-135) still in service. Almost all civilian 707s now retired.",
            specs = "Passengers: 140-219 | Range: 9,265km | Cruise: Mach 0.80 | Engines: 4x JT3D",
            photoAsset = "aircraft/B703.jpg",
            icaoTypeCodes = listOf("B703")
        ),

        // ══════════════════════════════════════
        // ── Widebody Airliners ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "b777",
            name = "Boeing 777-200/300ER",
            manufacturer = "Boeing",
            category = AircraftCategory.WIDEBODY,
            description = "World's largest twin-engine jet. The 777-300ER is the long-haul workhorse of major airlines. Recognizable by massive GE90 engines, six-wheel main gear bogies, and blade-shaped tail.",
            specs = "Passengers: 301-396 | Range: 13,650km | Cruise: Mach 0.84 | Engines: 2x GE90/Trent 800",
            photoAsset = "aircraft/B77W.jpg",
            icaoTypeCodes = listOf("B772", "B77W", "B77L")
        ),
        AircraftReference(
            id = "b787",
            name = "Boeing 787 Dreamliner",
            manufacturer = "Boeing",
            category = AircraftCategory.WIDEBODY,
            description = "Revolutionary composite-fuselage widebody with raked wingtips and larger windows. Electric architecture replaces bleed air. Opened hundreds of new point-to-point long-haul routes.",
            specs = "Passengers: 242-330 | Range: 14,140km | Cruise: Mach 0.85 | Engines: 2x GEnx/Trent 1000",
            photoAsset = "aircraft/B788.jpg",
            icaoTypeCodes = listOf("B788", "B789", "B78X")
        ),
        AircraftReference(
            id = "a330",
            name = "Airbus A330-200/300",
            manufacturer = "Airbus",
            category = AircraftCategory.WIDEBODY,
            description = "Versatile twin-aisle workhorse used on medium to long-haul routes. Shares cockpit type rating with A340. Popular as both passenger and freighter (A330-200F). Over 1,800 delivered.",
            specs = "Passengers: 247-440 | Range: 13,400km | Cruise: Mach 0.82 | Engines: 2x CF6/Trent 700/PW4000",
            photoAsset = "aircraft/A332.jpg",
            icaoTypeCodes = listOf("A332", "A333")
        ),
        AircraftReference(
            id = "a330neo",
            name = "Airbus A330-900neo",
            manufacturer = "Airbus",
            category = AircraftCategory.WIDEBODY,
            description = "Re-engined A330 with Trent 7000 engines and 3D-printed sharklet wingtips. Competes with Boeing 787. Shares A350 cabin design features. Lower operating costs than original A330.",
            specs = "Passengers: 260-440 | Range: 13,334km | Cruise: Mach 0.82 | Engines: 2x Trent 7000",
            photoAsset = "aircraft/A339.jpg",
            icaoTypeCodes = listOf("A339")
        ),
        AircraftReference(
            id = "a340",
            name = "Airbus A340",
            manufacturer = "Airbus",
            category = AircraftCategory.WIDEBODY,
            description = "Four-engine long-range widebody. The only quad-jet Airbus produced. Distinguished from A330 by four engines and main gear sponson. Largely retired due to fuel inefficiency.",
            specs = "Passengers: 261-440 | Range: 16,670km | Cruise: Mach 0.82 | Engines: 4x CFM56/Trent 500",
            photoAsset = "aircraft/A343.jpg",
            icaoTypeCodes = listOf("A342", "A343", "A345", "A346")
        ),
        AircraftReference(
            id = "a350",
            name = "Airbus A350 XWB",
            manufacturer = "Airbus",
            category = AircraftCategory.WIDEBODY,
            description = "Airbus's newest widebody with 53% composite airframe. Distinctive 'raccoon mask' cockpit windows. Direct competitor to Boeing 787 and 777X. Extremely quiet cabin.",
            specs = "Passengers: 300-440 | Range: 16,100km | Cruise: Mach 0.85 | Engines: 2x Trent XWB",
            photoAsset = "aircraft/A359.jpg",
            icaoTypeCodes = listOf("A359", "A35K")
        ),
        AircraftReference(
            id = "a380",
            name = "Airbus A380",
            manufacturer = "Airbus",
            category = AircraftCategory.WIDEBODY,
            description = "World's largest passenger aircraft with full-length upper deck. Double-decker superjumbo carrying 500+ passengers. Production ended 2021 but remains flying with Emirates, Singapore, and others.",
            specs = "Passengers: 525-853 | Range: 14,800km | Cruise: Mach 0.85 | Engines: 4x Trent 900/GP7200",
            photoAsset = "aircraft/A388.jpg",
            icaoTypeCodes = listOf("A388")
        ),
        AircraftReference(
            id = "b747",
            name = "Boeing 747",
            manufacturer = "Boeing",
            category = AircraftCategory.WIDEBODY,
            description = "The original 'Jumbo Jet' and Queen of the Skies. Distinctive hump upper deck. Revolutionized air travel in 1970. The 747-8 is the latest and final variant. Still flying as freighter and VIP transport.",
            specs = "Passengers: 410-524 | Range: 14,320km | Cruise: Mach 0.855 | Engines: 4x GEnx/CF6/RB211",
            photoAsset = "aircraft/B744.jpg",
            icaoTypeCodes = listOf("B744", "B748")
        ),
        AircraftReference(
            id = "b767",
            name = "Boeing 767",
            manufacturer = "Boeing",
            category = AircraftCategory.WIDEBODY,
            description = "Mid-size widebody that pioneered ETOPS transatlantic twin-engine operations. Common as freighter (FedEx, UPS, Amazon). Also serves as KC-46 tanker and E-767 AWACS platform.",
            specs = "Passengers: 181-375 | Range: 11,070km | Cruise: Mach 0.80 | Engines: 2x CF6/JT9D/PW4000",
            photoAsset = "aircraft/B763.jpg",
            icaoTypeCodes = listOf("B763", "B764")
        ),
        AircraftReference(
            id = "md11",
            name = "McDonnell Douglas MD-11",
            manufacturer = "McDonnell Douglas",
            category = AircraftCategory.WIDEBODY,
            description = "Three-engine widebody derived from DC-10. Distinctive winglets and smaller tail engine intake. All passenger versions retired; still flying as freighter with FedEx and Western Global.",
            specs = "Passengers: 293-410 | Range: 12,270km | Cruise: Mach 0.82 | Engines: 3x CF6/PW4000",
            photoAsset = "aircraft/MD11.jpg",
            icaoTypeCodes = listOf("MD11")
        ),
        AircraftReference(
            id = "dc10",
            name = "McDonnell Douglas DC-10",
            manufacturer = "McDonnell Douglas",
            category = AircraftCategory.WIDEBODY,
            description = "Iconic tri-jet widebody from the 1970s. Number-2 engine mounted in the vertical tail. Once plagued by design controversies but became reliable workhorse. Military version is KC-10 tanker.",
            specs = "Passengers: 250-380 | Range: 9,600km | Cruise: Mach 0.82 | Engines: 3x CF6",
            photoAsset = "aircraft/DC10.jpg",
            icaoTypeCodes = listOf("DC10")
        ),
        AircraftReference(
            id = "a300_a310",
            name = "Airbus A300/A310",
            manufacturer = "Airbus",
            category = AircraftCategory.WIDEBODY,
            description = "The original Airbus — world's first twin-engine widebody. The A310 is a shortened derivative. Mostly retired from passenger service; still operating as freighters and military transports.",
            specs = "Passengers: 220-361 | Range: 7,700km | Cruise: Mach 0.78 | Engines: 2x CF6/JT9D/PW4000",
            photoAsset = "aircraft/A306.jpg",
            icaoTypeCodes = listOf("A306", "A30B", "A310")
        ),
        AircraftReference(
            id = "su95",
            name = "Sukhoi Superjet 100",
            manufacturer = "Sukhoi (Russia)",
            category = AircraftCategory.REGIONAL,
            description = "Russian regional jet designed with Western partners (PowerJet SaM146 engines). Used mainly by Aeroflot and other Russian carriers. New SSJ-100R variant aims for import substitution.",
            specs = "Passengers: 87-108 | Range: 4,578km | Cruise: Mach 0.78 | Engines: 2x SaM146",
            photoAsset = "aircraft/SU95.jpg",
            icaoTypeCodes = listOf("SU95")
        ),
        AircraftReference(
            id = "bae146",
            name = "BAe 146 / Avro RJ",
            manufacturer = "British Aerospace",
            category = AircraftCategory.REGIONAL,
            description = "Quiet four-engine regional jet nicknamed 'Whisper Jet.' High wing with four ALF502 engines. Exceptional short-field performance. Used as VIP transport and firefighting tanker.",
            specs = "Passengers: 70-128 | Range: 2,909km | Cruise: Mach 0.70 | Engines: 4x ALF502/LF507",
            photoAsset = "aircraft/RJ85.jpg",
            icaoTypeCodes = listOf("RJ85", "RJ1H")
        ),

        // ══════════════════════════════════════
        // ── Regional Jets ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "crj200",
            name = "Bombardier CRJ-200",
            manufacturer = "Bombardier",
            category = AircraftCategory.REGIONAL,
            description = "50-seat regional jet that defined the RJ category. Rear-mounted engines and T-tail. Once the most common regional jet in North America. Being retired in favor of larger types.",
            specs = "Passengers: 50 | Range: 3,148km | Cruise: Mach 0.78 | Engines: 2x CF34-3A1",
            photoAsset = "aircraft/CRJ2.jpg",
            icaoTypeCodes = listOf("CRJ2")
        ),
        AircraftReference(
            id = "crj700_900",
            name = "Bombardier CRJ-700/900/1000",
            manufacturer = "Bombardier",
            category = AircraftCategory.REGIONAL,
            description = "Stretched CRJ family for 70-104 passengers. Longer fuselage and improved wings over CRJ-200. Common with SkyWest, Endeavor, and Mesa Airlines under major carrier brands.",
            specs = "Passengers: 70-104 | Range: 2,956km | Cruise: Mach 0.78 | Engines: 2x CF34-8C5",
            photoAsset = "aircraft/CRJ9.jpg",
            icaoTypeCodes = listOf("CRJ7", "CRJ9", "CRJX")
        ),
        AircraftReference(
            id = "e170",
            name = "Embraer E170",
            manufacturer = "Embraer",
            category = AircraftCategory.REGIONAL,
            description = "First of the E-Jet family with double-bubble fuselage cross-section. 2-2 seating eliminates middle seats. Popular with regional carriers and Republic Airways.",
            specs = "Passengers: 66-78 | Range: 3,889km | Cruise: Mach 0.78 | Engines: 2x CF34-8E",
            photoAsset = "aircraft/E170.jpg",
            icaoTypeCodes = listOf("E170")
        ),
        AircraftReference(
            id = "e175",
            name = "Embraer E175",
            manufacturer = "Embraer",
            category = AircraftCategory.REGIONAL,
            description = "Most popular E-Jet variant and dominant 76-seat regional jet in North America. Scope clause favorite due to 76-seat / MTOW limits. Over 800 delivered.",
            specs = "Passengers: 76-88 | Range: 3,704km | Cruise: Mach 0.78 | Engines: 2x CF34-8E",
            photoAsset = "aircraft/E75L.jpg",
            icaoTypeCodes = listOf("E75L", "E75S")
        ),
        AircraftReference(
            id = "e190_195",
            name = "Embraer E190/E195",
            manufacturer = "Embraer",
            category = AircraftCategory.REGIONAL,
            description = "Larger E-Jet variants bridging regional and mainline. JetBlue made the E190 famous. 2-2 seating, generous overhead bins, and quiet cabin. Competes with A220.",
            specs = "Passengers: 97-132 | Range: 4,537km | Cruise: Mach 0.78 | Engines: 2x CF34-10E",
            photoAsset = "aircraft/E190.jpg",
            icaoTypeCodes = listOf("E190", "E195")
        ),
        AircraftReference(
            id = "e2",
            name = "Embraer E190-E2/E195-E2",
            manufacturer = "Embraer",
            category = AircraftCategory.REGIONAL,
            description = "Second-generation E-Jets with new PW1900G geared turbofan engines and redesigned wing. 25% fuel improvement. Nicknamed 'Profit Hunter' with distinctive shark-themed livery.",
            specs = "Passengers: 97-146 | Range: 5,278km | Cruise: Mach 0.78 | Engines: 2x PW1900G",
            photoAsset = "aircraft/E290.jpg",
            icaoTypeCodes = listOf("E290", "E295")
        ),
        AircraftReference(
            id = "erj",
            name = "Embraer ERJ-135/145",
            manufacturer = "Embraer",
            category = AircraftCategory.REGIONAL,
            description = "First-generation Embraer regional jets. Slender fuselage with 1-2 seating. The 37-seat ERJ-135 and 50-seat ERJ-145 served as workhorses for regional carriers. Military variant is the E-99 AEW.",
            specs = "Passengers: 37-50 | Range: 3,019km | Cruise: Mach 0.78 | Engines: 2x AE3007",
            photoAsset = "aircraft/E145.jpg",
            icaoTypeCodes = listOf("E135", "E145")
        ),
        AircraftReference(
            id = "saab_340",
            name = "Saab 340/2000",
            manufacturer = "Saab",
            category = AircraftCategory.REGIONAL,
            description = "Swedish turboprop-powered regional aircraft. The Saab 340 seats 34-37 passengers, while the stretched Saab 2000 carries 50-58. Still in service with smaller operators.",
            specs = "Passengers: 34-58 | Range: 1,500-2,868km | Cruise: 522km/h | Engines: 2x CT7/AE2100",
            photoAsset = "aircraft/SF34.jpg",
            icaoTypeCodes = listOf("SF34", "SB20")
        ),

        // ══════════════════════════════════════
        // ── Turboprops ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "atr42",
            name = "ATR 42",
            manufacturer = "ATR (Airbus/Leonardo)",
            category = AircraftCategory.TURBOPROP,
            description = "Short-range turboprop for 42-50 passengers. High wing, PW120 engines. Popular for island hopping and thin regional routes in Europe, Asia, and the Caribbean.",
            specs = "Passengers: 42-50 | Range: 1,326km | Cruise: 556km/h | Engines: 2x PW120",
            photoAsset = "aircraft/AT43.jpg",
            icaoTypeCodes = listOf("AT43")
        ),
        AircraftReference(
            id = "atr72",
            name = "ATR 72",
            manufacturer = "ATR (Airbus/Leonardo)",
            category = AircraftCategory.TURBOPROP,
            description = "Stretched ATR 42 seating 70-78 passengers. Most popular turboprop in production. Identifiable by high wing, twin PW127 engines, and distinctive landing gear fairings.",
            specs = "Passengers: 70-78 | Range: 1,528km | Cruise: 510km/h | Engines: 2x PW127",
            photoAsset = "aircraft/AT72.jpg",
            icaoTypeCodes = listOf("AT72", "AT76")
        ),
        AircraftReference(
            id = "dash8_100_200",
            name = "De Havilland Dash 8-100/200",
            manufacturer = "De Havilland Canada",
            category = AircraftCategory.TURBOPROP,
            description = "Short-haul turboprop designed for STOL operations. High wing with PW120A engines. The -200 has more powerful engines and better hot-and-high performance.",
            specs = "Passengers: 37-39 | Range: 1,889km | Cruise: 500km/h | Engines: 2x PW120A/PW123",
            photoAsset = "aircraft/DH8A.jpg",
            icaoTypeCodes = listOf("DH8A", "DH8B")
        ),
        AircraftReference(
            id = "dash8_300_400",
            name = "De Havilland Dash 8-300/400 (Q400)",
            manufacturer = "De Havilland Canada",
            category = AircraftCategory.TURBOPROP,
            description = "Stretched Dash 8. The Q400 is the fastest turboprop in regular service at 667km/h. Active noise and vibration suppression system. Used by Alaska/Horizon, Porter, and many others.",
            specs = "Passengers: 50-90 | Range: 2,040km | Cruise: 667km/h | Engines: 2x PW150A",
            photoAsset = "aircraft/DH8D.jpg",
            icaoTypeCodes = listOf("DH8C", "DH8D")
        ),
        AircraftReference(
            id = "king_air",
            name = "Beechcraft King Air",
            manufacturer = "Beechcraft/Textron",
            category = AircraftCategory.TURBOPROP,
            description = "Best-selling turboprop family in history with 7,600+ delivered since 1964. Used by military, medical, cargo, and executive operators worldwide. PT6A engines are legendarily reliable.",
            specs = "Passengers: 7-15 | Range: 3,185km | Cruise: 536km/h | Engines: 2x PT6A",
            photoAsset = "aircraft/BE20.jpg",
            icaoTypeCodes = listOf("BE20", "BE30")
        ),
        AircraftReference(
            id = "c208",
            name = "Cessna 208 Caravan",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.TURBOPROP,
            description = "Rugged single-engine utility turboprop. Operates from dirt strips, floats, and skis. Used by FedEx feeders, bush operators, skydiving operations, and missionaries worldwide.",
            specs = "Passengers: 9-14 | Range: 1,982km | Cruise: 344km/h | Engine: 1x PT6A-114A",
            photoAsset = "aircraft/C208.jpg",
            icaoTypeCodes = listOf("C208")
        ),
        AircraftReference(
            id = "pc12",
            name = "Pilatus PC-12",
            manufacturer = "Pilatus",
            category = AircraftCategory.TURBOPROP,
            description = "Swiss-made single-engine turboprop known for versatility. Pressurized cabin, cargo door, and excellent short-field performance. Popular executive transport and air ambulance.",
            specs = "Passengers: 6-9 | Range: 3,417km | Cruise: 528km/h | Engine: 1x PT6A-67P",
            photoAsset = "aircraft/PC12.jpg",
            icaoTypeCodes = listOf("PC12")
        ),
        AircraftReference(
            id = "sw4",
            name = "Fairchild Metroliner/Merlin",
            manufacturer = "Fairchild/Swearingen",
            category = AircraftCategory.TURBOPROP,
            description = "19-seat commuter turboprop with distinctive long, narrow fuselage. Known for noisy cabin but excellent utility. Many now used as cargo feeders.",
            specs = "Passengers: 19 | Range: 2,130km | Cruise: 515km/h | Engines: 2x TPE331",
            photoAsset = "aircraft/SW4.jpg",
            icaoTypeCodes = listOf("SW4")
        ),
        AircraftReference(
            id = "tbm",
            name = "Daher TBM 700/850/900/960",
            manufacturer = "Daher",
            category = AircraftCategory.TURBOPROP,
            description = "World's fastest single-engine turboprop at 611km/h. French-made pressurized six-seater. Owner-flown favorite for its speed, efficiency, and autothrottle/autoland capability (TBM 960).",
            specs = "Passengers: 6 | Range: 3,352km | Cruise: 611km/h | Engine: 1x PT6A-66D",
            photoAsset = "aircraft/TBM9.jpg",
            icaoTypeCodes = listOf("TBM7", "TBM8", "TBM9")
        ),
        AircraftReference(
            id = "pc6",
            name = "Pilatus PC-6 Porter",
            manufacturer = "Pilatus",
            category = AircraftCategory.TURBOPROP,
            description = "STOL utility aircraft built for mountain and bush operations. Can operate from incredibly short runways (200m). Used for skydiving, military, and humanitarian missions.",
            specs = "Passengers: 6-10 | Range: 730km | Cruise: 222km/h | Engine: 1x PT6A-27",
            photoAsset = "aircraft/PC6.jpg",
            icaoTypeCodes = listOf("PC6")
        ),
        AircraftReference(
            id = "p46t",
            name = "Piper Malibu Meridian/M500/M600",
            manufacturer = "Piper",
            category = AircraftCategory.TURBOPROP,
            description = "Single-engine pressurized turboprop evolved from the piston Malibu. The M600/SLS features Garmin Autoland — can land the plane autonomously in emergencies.",
            specs = "Passengers: 5-6 | Range: 2,778km | Cruise: 483km/h | Engine: 1x PT6A-42A",
            photoAsset = "aircraft/P46T.jpg",
            icaoTypeCodes = listOf("P46T")
        ),

        // ══════════════════════════════════════
        // ── Business Jets ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "gulfstream_large",
            name = "Gulfstream G450/G500/G550/G600/G650/G700",
            manufacturer = "Gulfstream",
            category = AircraftCategory.BIZJET,
            description = "Ultra-long-range business jets. The G650 can fly 12,964km nonstop. Oval fuselage windows, T-tail (older) or low tail (newer). Favored by heads of state and Fortune 500.",
            specs = "Passengers: 12-19 | Range: 12,964km | Cruise: Mach 0.85 | Engines: 2x BR725/PW800",
            photoAsset = "aircraft/GLF6.jpg",
            icaoTypeCodes = listOf("GLF4", "GLF5", "GLF6", "GLEX", "GA5C", "GA6C")
        ),
        AircraftReference(
            id = "g280",
            name = "Gulfstream G280",
            manufacturer = "Gulfstream (IAI)",
            category = AircraftCategory.BIZJET,
            description = "Super-midsize business jet built by Israel Aerospace Industries for Gulfstream. Known for steep approach capability, allowing access to London City Airport.",
            specs = "Passengers: 10 | Range: 6,667km | Cruise: Mach 0.80 | Engines: 2x HTF7250G",
            photoAsset = "aircraft/G280.jpg",
            icaoTypeCodes = listOf("G280")
        ),
        AircraftReference(
            id = "challenger",
            name = "Bombardier Challenger 300/350/600/650",
            manufacturer = "Bombardier",
            category = AircraftCategory.BIZJET,
            description = "Wide-cabin business jets. The Challenger 350 is the best-selling super-midsize jet ever. The 650 is a large-cabin transcontinental jet based on the original Challenger 600.",
            specs = "Passengers: 8-12 | Range: 5,926-7,408km | Cruise: Mach 0.80 | Engines: 2x HTF7350/CF34",
            photoAsset = "aircraft/CL35.jpg",
            icaoTypeCodes = listOf("CL30", "CL35", "CL60")
        ),
        AircraftReference(
            id = "citation_large",
            name = "Cessna Citation Sovereign/Latitude/Longitude",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.BIZJET,
            description = "Mid-to-super-midsize Citation jets. The Longitude is Cessna's flagship with flat-floor cabin. Known for reliability, low operating costs, and single-pilot certification.",
            specs = "Passengers: 8-12 | Range: 5,556-6,482km | Cruise: Mach 0.80 | Engines: 2x PW306D/Silvercrest",
            photoAsset = "aircraft/C680.jpg",
            icaoTypeCodes = listOf("C56X", "C560", "C680", "C700")
        ),
        AircraftReference(
            id = "citation_cj",
            name = "Cessna Citation CJ1/CJ2/CJ3/CJ4",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.BIZJET,
            description = "Light Citation jets evolved from the original CitationJet. Owner-flown favorites with single-pilot certification. CJ4 is the largest, seating 10 with 3,700km range.",
            specs = "Passengers: 5-10 | Range: 2,593-3,704km | Cruise: Mach 0.72 | Engines: 2x FJ44/Williams FJ44",
            photoAsset = "aircraft/C525.jpg",
            icaoTypeCodes = listOf("C525", "C25A", "C25B")
        ),
        AircraftReference(
            id = "citation_mustang",
            name = "Cessna Citation Mustang",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.BIZJET,
            description = "Entry-level very light jet (VLJ). Four-seat cabin with single-pilot ops. Affordable jet ownership. Production ended 2017 but active on the used market.",
            specs = "Passengers: 4 | Range: 2,130km | Cruise: Mach 0.63 | Engines: 2x PW615F",
            photoAsset = "aircraft/C510.jpg",
            icaoTypeCodes = listOf("C510")
        ),
        AircraftReference(
            id = "learjet",
            name = "Learjet 35/45/60/75",
            manufacturer = "Bombardier (Learjet)",
            category = AircraftCategory.BIZJET,
            description = "Iconic light-to-midsize business jet brand. Known for speed and sportiness. Production ended 2021. The Learjet name is synonymous with private aviation. Military C-21A based on Learjet 35.",
            specs = "Passengers: 6-9 | Range: 3,167-4,444km | Cruise: Mach 0.78 | Engines: 2x TFE731/AS907",
            photoAsset = "aircraft/LJ45.jpg",
            icaoTypeCodes = listOf("LJ35", "LJ45", "LJ60")
        ),
        AircraftReference(
            id = "falcon_large",
            name = "Dassault Falcon 7X/8X/900/2000",
            manufacturer = "Dassault",
            category = AircraftCategory.BIZJET,
            description = "French tri-jet business aircraft. The 7X/8X have three Pratt & Whitney engines and digital fly-by-wire (first in business aviation). Known for excellent range and short-field performance.",
            specs = "Passengers: 12-16 | Range: 11,945km | Cruise: Mach 0.80 | Engines: 3x PW307/Silvercrest",
            photoAsset = "aircraft/FA8X.jpg",
            icaoTypeCodes = listOf("FA7X", "FA8X", "F900", "F2TH")
        ),
        AircraftReference(
            id = "phenom300",
            name = "Embraer Phenom 300/300E",
            manufacturer = "Embraer",
            category = AircraftCategory.BIZJET,
            description = "World's most-delivered light business jet for a decade running. Brazilian-made with best-in-class performance. Popular for charter and owner-flown operations.",
            specs = "Passengers: 6-10 | Range: 3,650km | Cruise: Mach 0.78 | Engines: 2x PW535E1",
            photoAsset = "aircraft/E55P.jpg",
            icaoTypeCodes = listOf("E55P")
        ),
        AircraftReference(
            id = "hondajet",
            name = "Honda HA-420 HondaJet",
            manufacturer = "Honda Aircraft",
            category = AircraftCategory.BIZJET,
            description = "Distinctive very light jet with over-the-wing engine mounts (OTWEM) — unique in aviation. Composite fuselage. Honda's first aircraft, it offers more cabin space than competitors.",
            specs = "Passengers: 5-6 | Range: 2,661km | Cruise: Mach 0.72 | Engines: 2x GE Honda HF120",
            photoAsset = "aircraft/HDJT.jpg",
            icaoTypeCodes = listOf("HDJT")
        ),
        AircraftReference(
            id = "hawker800",
            name = "Hawker 800/900XP",
            manufacturer = "Hawker Beechcraft",
            category = AircraftCategory.BIZJET,
            description = "British-designed midsize business jet evolved from the de Havilland 125. Known as 'BAe 125' in military service. Robust and popular in charter fleet. Production ended but many active.",
            specs = "Passengers: 8-15 | Range: 5,186km | Cruise: Mach 0.80 | Engines: 2x TFE731-5BR",
            photoAsset = "aircraft/H25B.jpg",
            icaoTypeCodes = listOf("H25B")
        ),
        AircraftReference(
            id = "beechjet400",
            name = "Beechcraft 400A/Hawker 400XP",
            manufacturer = "Beechcraft/Raytheon",
            category = AircraftCategory.BIZJET,
            description = "Light business jet derived from Mitsubishi Diamond. Military version is the T-1A Jayhawk trainer for USAF. Economical to operate with JT15D engines.",
            specs = "Passengers: 7-9 | Range: 2,593km | Cruise: Mach 0.78 | Engines: 2x JT15D-5",
            photoAsset = "aircraft/BE40.jpg",
            icaoTypeCodes = listOf("BE40")
        ),
        AircraftReference(
            id = "prm1",
            name = "Raytheon/Beechcraft Premier I",
            manufacturer = "Beechcraft/Raytheon",
            category = AircraftCategory.BIZJET,
            description = "Light jet with composite fuselage — first business jet with all-composite airframe. T-tail design. Known for spacious cabin relative to its class. Single-pilot certified.",
            specs = "Passengers: 6 | Range: 2,519km | Cruise: Mach 0.80 | Engines: 2x FJ44-2A",
            photoAsset = "aircraft/PRM1.jpg",
            icaoTypeCodes = listOf("PRM1")
        ),
        AircraftReference(
            id = "legacy450",
            name = "Embraer Legacy 450/500 (Praetor 500/600)",
            manufacturer = "Embraer",
            category = AircraftCategory.BIZJET,
            description = "Super-midsize jets rebranded as Praetor 500/600. Fly-by-wire flight controls (unique in this class). Flat-floor cabin and full lavatory. The Praetor 600 offers intercontinental range.",
            specs = "Passengers: 8-12 | Range: 6,019km | Cruise: Mach 0.82 | Engines: 2x HTF7500E",
            photoAsset = "aircraft/E545.jpg",
            icaoTypeCodes = listOf("E545")
        ),

        // ══════════════════════════════════════
        // ── Helicopters ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "r44",
            name = "Robinson R44",
            manufacturer = "Robinson",
            category = AircraftCategory.HELICOPTER,
            description = "World's most popular light helicopter. Piston-powered, four-seat. Ubiquitous for flight training, sightseeing, and personal transport. Affordable to buy and maintain.",
            specs = "Passengers: 3 | Range: 563km | Cruise: 209km/h | Engine: 1x Lycoming IO-540",
            photoAsset = "aircraft/R44.jpg",
            icaoTypeCodes = listOf("R44")
        ),
        AircraftReference(
            id = "r22",
            name = "Robinson R22",
            manufacturer = "Robinson",
            category = AircraftCategory.HELICOPTER,
            description = "Two-seat light helicopter and the world's most common training helicopter. Extremely economical but requires skilled handling. Over 4,800 delivered.",
            specs = "Passengers: 1 | Range: 463km | Cruise: 177km/h | Engine: 1x Lycoming O-360",
            photoAsset = "aircraft/R22.jpg",
            icaoTypeCodes = listOf("R22")
        ),
        AircraftReference(
            id = "ec135",
            name = "Airbus H135 (EC135)",
            manufacturer = "Airbus Helicopters",
            category = AircraftCategory.HELICOPTER,
            description = "Light twin-engine helicopter. Leading air ambulance and law enforcement helicopter in Europe and North America. Fenestron enclosed tail rotor for safety.",
            specs = "Passengers: 6-7 | Range: 620km | Cruise: 254km/h | Engines: 2x PW206/Arrius",
            photoAsset = "aircraft/EC35.jpg",
            icaoTypeCodes = listOf("EC35")
        ),
        AircraftReference(
            id = "ec145",
            name = "Airbus H145 (EC145)",
            manufacturer = "Airbus Helicopters",
            category = AircraftCategory.HELICOPTER,
            description = "Medium twin-engine helicopter used extensively for HEMS, law enforcement, and military. Latest D3 version has five-blade bearingless main rotor. US Army version is UH-72 Lakota.",
            specs = "Passengers: 8-9 | Range: 680km | Cruise: 248km/h | Engines: 2x Arriel 2E",
            photoAsset = "aircraft/EC45.jpg",
            icaoTypeCodes = listOf("EC45")
        ),
        AircraftReference(
            id = "ec130",
            name = "Airbus H130 (EC130)",
            manufacturer = "Airbus Helicopters",
            category = AircraftCategory.HELICOPTER,
            description = "Single-engine helicopter with wide cabin and panoramic windows. Popular for tourism flights (Grand Canyon tours). Fenestron tail rotor reduces noise.",
            specs = "Passengers: 6-7 | Range: 616km | Cruise: 236km/h | Engine: 1x Arriel 2D",
            photoAsset = "aircraft/EC30.jpg",
            icaoTypeCodes = listOf("EC30")
        ),
        AircraftReference(
            id = "as350",
            name = "Airbus H125 (AS350 Ecureuil)",
            manufacturer = "Airbus Helicopters",
            category = AircraftCategory.HELICOPTER,
            description = "World's most popular single-engine turbine helicopter. 'Squirrel' in French. Used for everything from news gathering to Everest summit landings. Over 6,500 delivered.",
            specs = "Passengers: 5-6 | Range: 637km | Cruise: 246km/h | Engine: 1x Arriel 2D",
            photoAsset = "aircraft/AS50.jpg",
            icaoTypeCodes = listOf("AS50")
        ),
        AircraftReference(
            id = "aw109",
            name = "Leonardo AW109",
            manufacturer = "Leonardo (Agusta)",
            category = AircraftCategory.HELICOPTER,
            description = "Sleek Italian light twin helicopter. Retractable landing gear gives it a distinctive streamlined look. Popular for VIP, EMS, and military roles.",
            specs = "Passengers: 6-7 | Range: 932km | Cruise: 285km/h | Engines: 2x PW207C",
            photoAsset = "aircraft/A109.jpg",
            icaoTypeCodes = listOf("A109")
        ),
        AircraftReference(
            id = "aw139",
            name = "Leonardo AW139",
            manufacturer = "Leonardo",
            category = AircraftCategory.HELICOPTER,
            description = "Medium twin-engine helicopter — world's best-selling in its class. Used by offshore oil, SAR, VIP, and military operators. PT6C engines and wide flat-floor cabin.",
            specs = "Passengers: 12-15 | Range: 1,061km | Cruise: 306km/h | Engines: 2x PT6C-67C",
            photoAsset = "aircraft/A139.jpg",
            icaoTypeCodes = listOf("A139")
        ),
        AircraftReference(
            id = "bell206",
            name = "Bell 206 JetRanger",
            manufacturer = "Bell",
            category = AircraftCategory.HELICOPTER,
            description = "Iconic light turbine helicopter since 1967. Over 8,400 built. Military version is OH-58 Kiowa. The definitive 'news chopper' and police helicopter silhouette.",
            specs = "Passengers: 4 | Range: 693km | Cruise: 213km/h | Engine: 1x Allison 250-C20J",
            photoAsset = "aircraft/B06.jpg",
            icaoTypeCodes = listOf("B06")
        ),
        AircraftReference(
            id = "bell407",
            name = "Bell 407",
            manufacturer = "Bell",
            category = AircraftCategory.HELICOPTER,
            description = "Light single-engine helicopter with four-blade composite rotor. Popular for law enforcement, EMS, corporate, and tour operations. Upgraded successor to the LongRanger.",
            specs = "Passengers: 6 | Range: 722km | Cruise: 246km/h | Engine: 1x Honeywell HTS900",
            photoAsset = "aircraft/B407.jpg",
            icaoTypeCodes = listOf("B407")
        ),
        AircraftReference(
            id = "bell429",
            name = "Bell 429 GlobalRanger",
            manufacturer = "Bell",
            category = AircraftCategory.HELICOPTER,
            description = "Light twin-engine helicopter with wide cabin and retractable gear option. Advanced glass cockpit. Used for HEMS, law enforcement, and corporate transport.",
            specs = "Passengers: 7 | Range: 722km | Cruise: 272km/h | Engines: 2x PW207D1",
            photoAsset = "aircraft/B429.jpg",
            icaoTypeCodes = listOf("B429")
        ),
        AircraftReference(
            id = "s76",
            name = "Sikorsky S-76",
            manufacturer = "Sikorsky/Lockheed Martin",
            category = AircraftCategory.HELICOPTER,
            description = "Medium twin-engine helicopter. Primary use is offshore oil transport and VIP/corporate. Known for smooth ride and reliability. Used as Marine One VIP transport.",
            specs = "Passengers: 12 | Range: 833km | Cruise: 287km/h | Engines: 2x Arriel 2S2/CT7",
            photoAsset = "aircraft/S76.jpg",
            icaoTypeCodes = listOf("S76")
        ),
        AircraftReference(
            id = "s92",
            name = "Sikorsky S-92",
            manufacturer = "Sikorsky/Lockheed Martin",
            category = AircraftCategory.HELICOPTER,
            description = "Heavy twin-engine helicopter designed for offshore oil, SAR, and head-of-state transport. Flies as VH-92A for US Presidential transport fleet. Seats 19 in utility configuration.",
            specs = "Passengers: 19 | Range: 1,000km | Cruise: 256km/h | Engines: 2x CT7-8A",
            photoAsset = "aircraft/S92.jpg",
            icaoTypeCodes = listOf("S92")
        ),
        AircraftReference(
            id = "bk117",
            name = "Airbus/Kawasaki BK117",
            manufacturer = "Airbus/Kawasaki",
            category = AircraftCategory.HELICOPTER,
            description = "Twin-engine helicopter jointly developed by MBB and Kawasaki. Clamshell rear doors provide stretcher access for EMS. Predecessor to the H145.",
            specs = "Passengers: 7-10 | Range: 541km | Cruise: 248km/h | Engines: 2x LTS101",
            photoAsset = "aircraft/BK17.jpg",
            icaoTypeCodes = listOf("BK17")
        ),
        AircraftReference(
            id = "uh60",
            name = "Sikorsky UH-60 Black Hawk",
            manufacturer = "Sikorsky/Lockheed Martin",
            category = AircraftCategory.HELICOPTER,
            description = "US Army's primary utility helicopter since 1979. Over 4,000 built in many variants: HH-60 (SAR), SH-60 (Navy), MH-60 (special ops). Recognizable by squared-off engine housings.",
            specs = "Crew: 2-4 | Range: 590km | Cruise: 280km/h | Engines: 2x T700-GE-701D",
            photoAsset = "aircraft/UH60.jpg",
            icaoTypeCodes = listOf("UH60", "H60")
        ),
        AircraftReference(
            id = "ch47",
            name = "Boeing CH-47 Chinook",
            manufacturer = "Boeing",
            category = AircraftCategory.HELICOPTER,
            description = "Tandem-rotor heavy-lift helicopter. Two counter-rotating rotors eliminate need for tail rotor. Can carry 33 troops or 10 tons of cargo. Over 60 years in service.",
            specs = "Crew: 3 + 33 troops | Range: 741km | Cruise: 291km/h | Engines: 2x T55-GA-714A",
            photoAsset = "aircraft/CH47.jpg",
            icaoTypeCodes = listOf("CH47", "H47")
        ),
        AircraftReference(
            id = "ah64",
            name = "Boeing AH-64 Apache",
            manufacturer = "Boeing",
            category = AircraftCategory.HELICOPTER,
            description = "Primary US Army attack helicopter. Twin-engine tandem-seat gunship with chain gun, Hellfire missiles, and Hydra rockets. AH-64E Guardian is the latest version.",
            specs = "Crew: 2 | Range: 476km | Cruise: 265km/h | Engines: 2x T700-GE-701D",
            photoAsset = "aircraft/AH64.jpg",
            icaoTypeCodes = listOf("AH64")
        ),
        AircraftReference(
            id = "uh1",
            name = "Bell UH-1 Iroquois (Huey)",
            manufacturer = "Bell",
            category = AircraftCategory.HELICOPTER,
            description = "The helicopter that defined the Vietnam War era. Distinctive 'wop-wop' two-blade rotor sound. Over 16,000 built. Still in military and civilian service worldwide. Evolved into Bell 212/412.",
            specs = "Passengers: 12-14 | Range: 510km | Cruise: 204km/h | Engine: 1x T53-L-13",
            photoAsset = "aircraft/UH1.jpg",
            icaoTypeCodes = listOf("UH1")
        ),
        AircraftReference(
            id = "v22",
            name = "Bell Boeing V-22 Osprey",
            manufacturer = "Bell/Boeing",
            category = AircraftCategory.HELICOPTER,
            description = "Tiltrotor aircraft that takes off like a helicopter and flies like a turboprop. Used by USMC, USAF, and Navy. Can carry 24 troops at 509km/h. Unique silhouette is unmistakable.",
            specs = "Crew: 4 + 24 troops | Range: 1,627km | Cruise: 509km/h | Engines: 2x T406-AD-400",
            photoAsset = "aircraft/V22.jpg",
            icaoTypeCodes = listOf("V22", "MV22", "CV22")
        ),

        // ── Russian / Adversary Helicopters ──
        AircraftReference(
            id = "mi8",
            name = "Mil Mi-8/17 Hip",
            manufacturer = "Mil (Russia)",
            category = AircraftCategory.HELICOPTER,
            description = "Most-produced helicopter in history with over 17,000 built. Medium twin-turbine transport and gunship workhorse. Used by 90+ countries for troop transport, medevac, and armed assault.",
            specs = "Passengers: 24 | Range: 580km | Cruise: 225km/h | Engines: 2x TV3-117",
            photoAsset = "aircraft/MI8.jpg",
            icaoTypeCodes = listOf("MI8", "MI17")
        ),
        AircraftReference(
            id = "mi24",
            name = "Mil Mi-24/35 Hind",
            manufacturer = "Mil (Russia)",
            category = AircraftCategory.HELICOPTER,
            description = "Iconic Soviet attack helicopter with troop compartment — the only gunship that can carry 8 soldiers. Heavily armored, tandem cockpit with chin-mounted gun turret. Proved devastating in Afghanistan.",
            specs = "Crew: 2-3 + 8 troops | Range: 450km | Cruise: 270km/h | Engines: 2x TV3-117",
            photoAsset = "aircraft/MI24.jpg",
            icaoTypeCodes = listOf("MI24")
        ),
        AircraftReference(
            id = "mi28",
            name = "Mil Mi-28 Havoc",
            manufacturer = "Mil (Russia)",
            category = AircraftCategory.HELICOPTER,
            description = "Dedicated attack helicopter designed to destroy tanks and armored vehicles. Tandem cockpit with armored crew seats. Mi-28NM 'Night Hunter' variant has millimeter-wave radar. Used extensively in Ukraine.",
            specs = "Crew: 2 | Range: 450km | Max speed: 324km/h | Engines: 2x TV3-117VMA",
            photoAsset = "aircraft/MI28.jpg",
            icaoTypeCodes = listOf("MI28")
        ),
        AircraftReference(
            id = "ka52",
            name = "Kamov Ka-52 Alligator",
            manufacturer = "Kamov (Russia)",
            category = AircraftCategory.HELICOPTER,
            description = "Coaxial-rotor attack helicopter with side-by-side seating — unique among combat helicopters. No tail rotor needed. Equipped with Vikhr anti-tank missiles. Heavily used in Ukraine conflict.",
            specs = "Crew: 2 | Range: 460km | Max speed: 310km/h | Engines: 2x TV3-117VMA",
            photoAsset = "aircraft/KA52.jpg",
            icaoTypeCodes = listOf("KA52")
        ),

        // ══════════════════════════════════════
        // ── Fighter / Military ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "f16",
            name = "General Dynamics F-16 Fighting Falcon",
            manufacturer = "Lockheed Martin",
            category = AircraftCategory.FIGHTER,
            description = "World's most produced fighter jet with over 4,600 built. Single-engine multirole fighter used by 25+ nations. Known for 9G capability and bubble canopy. Still in production as F-16V Block 70/72.",
            specs = "Crew: 1-2 | Max speed: Mach 2.0 | Range: 3,220km | Engine: 1x F100/F110",
            photoAsset = "aircraft/F16.jpg",
            icaoTypeCodes = listOf("F16")
        ),
        AircraftReference(
            id = "f15",
            name = "McDonnell Douglas F-15 Eagle/Strike Eagle",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Undefeated air superiority fighter with 104-0 combat record. Twin-engine, twin-tail design. The F-15E Strike Eagle variant excels at ground attack. Still in production as F-15EX Eagle II.",
            specs = "Crew: 1-2 | Max speed: Mach 2.5 | Range: 5,550km | Engines: 2x F100-PW-229",
            photoAsset = "aircraft/F15.jpg",
            icaoTypeCodes = listOf("F15")
        ),
        AircraftReference(
            id = "fa18",
            name = "Boeing F/A-18 Hornet/Super Hornet",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "US Navy's primary carrier-based fighter. The Super Hornet (E/F) is 25% larger than the Legacy Hornet (A-D). EA-18G Growler is the electronic warfare variant. Blue Angels fly F/A-18s.",
            specs = "Crew: 1-2 | Max speed: Mach 1.8 | Range: 2,346km | Engines: 2x F414-GE-400",
            photoAsset = "aircraft/FA18.jpg",
            icaoTypeCodes = listOf("F18", "FA18", "EA18")
        ),
        AircraftReference(
            id = "f22",
            name = "Lockheed Martin F-22 Raptor",
            manufacturer = "Lockheed Martin",
            category = AircraftCategory.FIGHTER,
            description = "World's first fifth-generation stealth air superiority fighter. Supercruise at Mach 1.5+ without afterburner. Only 195 built, exclusively for USAF. Export prohibited by law.",
            specs = "Crew: 1 | Max speed: Mach 2.25 | Range: 2,960km | Engines: 2x F119-PW-100",
            photoAsset = "aircraft/F22.jpg",
            icaoTypeCodes = listOf("F22")
        ),
        AircraftReference(
            id = "f35",
            name = "Lockheed Martin F-35 Lightning II",
            manufacturer = "Lockheed Martin",
            category = AircraftCategory.FIGHTER,
            description = "Stealth multirole fighter in three variants: F-35A (conventional), F-35B (STOVL), F-35C (carrier). Most expensive weapons program in history. Ordered by 15+ nations.",
            specs = "Crew: 1 | Max speed: Mach 1.6 | Range: 2,220km | Engine: 1x F135-PW-100/600",
            photoAsset = "aircraft/F35.jpg",
            icaoTypeCodes = listOf("F35")
        ),
        AircraftReference(
            id = "eurofighter",
            name = "Eurofighter Typhoon",
            manufacturer = "Eurofighter (BAE/Airbus/Leonardo)",
            category = AircraftCategory.FIGHTER,
            description = "European twin-engine canard-delta multirole fighter. Operated by Germany, UK, Italy, Spain, and exported to Saudi Arabia, Kuwait, Qatar, and others. Supercruise capable.",
            specs = "Crew: 1-2 | Max speed: Mach 2.0 | Range: 2,900km | Engines: 2x EJ200",
            photoAsset = "aircraft/EUFI.jpg",
            icaoTypeCodes = listOf("EUFI")
        ),
        AircraftReference(
            id = "rafale",
            name = "Dassault Rafale",
            manufacturer = "Dassault",
            category = AircraftCategory.FIGHTER,
            description = "French twin-engine canard-delta omnirole fighter. Carrier-capable (Rafale M). Proven in combat over Libya, Mali, Syria, and Iraq. Export success with India, Egypt, UAE, and others.",
            specs = "Crew: 1-2 | Max speed: Mach 1.8 | Range: 3,700km | Engines: 2x M88-2",
            photoAsset = "aircraft/RFAL.jpg",
            icaoTypeCodes = listOf("RFAL")
        ),
        AircraftReference(
            id = "b1",
            name = "Rockwell B-1B Lancer",
            manufacturer = "Rockwell/Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Supersonic variable-sweep wing strategic bomber. Four GE F101 engines. Carries the largest payload of any US bomber. Used extensively for precision strike since 2001. Being replaced by B-21.",
            specs = "Crew: 4 | Max speed: Mach 1.25 | Range: 11,998km | Engines: 4x F101-GE-102",
            photoAsset = "aircraft/B1.jpg",
            icaoTypeCodes = listOf("B1")
        ),
        AircraftReference(
            id = "b2",
            name = "Northrop Grumman B-2 Spirit",
            manufacturer = "Northrop Grumman",
            category = AircraftCategory.FIGHTER,
            description = "Flying-wing stealth bomber, one of the most iconic aircraft ever built. Only 21 produced at $2 billion each. Can deliver nuclear or conventional weapons anywhere in the world.",
            specs = "Crew: 2 | Cruise: Mach 0.95 | Range: 11,100km | Engines: 4x F118-GE-100",
            photoAsset = "aircraft/B2.jpg",
            icaoTypeCodes = listOf("B2")
        ),
        AircraftReference(
            id = "b52",
            name = "Boeing B-52 Stratofortress",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Eight-engine strategic bomber in continuous service since 1955. Expected to serve until 2050s — nearly 100 years. Carries cruise missiles, gravity bombs, and mines. Distinctive eight-engine silhouette.",
            specs = "Crew: 5 | Cruise: Mach 0.84 | Range: 14,080km | Engines: 8x TF33-PW-103",
            photoAsset = "aircraft/B52.jpg",
            icaoTypeCodes = listOf("B52")
        ),
        AircraftReference(
            id = "a10",
            name = "Fairchild Republic A-10 Thunderbolt II",
            manufacturer = "Fairchild Republic",
            category = AircraftCategory.FIGHTER,
            description = "Close air support 'tank killer' built around the GAU-8 30mm rotary cannon. Nicknamed 'Warthog.' Twin engines mounted high for survivability. Legendary toughness in combat.",
            specs = "Crew: 1 | Max speed: 706km/h | Range: 4,154km | Engines: 2x TF34-GE-100A",
            photoAsset = "aircraft/A10.jpg",
            icaoTypeCodes = listOf("A10")
        ),
        AircraftReference(
            id = "f117",
            name = "Lockheed F-117 Nighthawk",
            manufacturer = "Lockheed (Skunk Works)",
            category = AircraftCategory.FIGHTER,
            description = "World's first operational stealth aircraft. Faceted design deflects radar. Used in Panama, Gulf War, Kosovo. Officially retired 2008 but still seen flying from Tonopah Test Range.",
            specs = "Crew: 1 | Max speed: Mach 0.92 | Range: 1,720km | Engines: 2x F404-GE-F1D2",
            photoAsset = "aircraft/F117.jpg",
            icaoTypeCodes = listOf("F117")
        ),
        AircraftReference(
            id = "av8b",
            name = "McDonnell Douglas AV-8B Harrier II",
            manufacturer = "McDonnell Douglas/BAE",
            category = AircraftCategory.FIGHTER,
            description = "STOVL (Short Takeoff/Vertical Landing) attack aircraft. Single Pegasus engine with rotating nozzles. Operated from small carriers and forward bases by USMC. Being replaced by F-35B.",
            specs = "Crew: 1-2 | Max speed: Mach 0.89 | Range: 2,000km | Engine: 1x Pegasus 11-61",
            photoAsset = "aircraft/AV8B.jpg",
            icaoTypeCodes = listOf("AV8B")
        ),
        AircraftReference(
            id = "e2c",
            name = "Northrop Grumman E-2 Hawkeye",
            manufacturer = "Northrop Grumman",
            category = AircraftCategory.FIGHTER,
            description = "Carrier-based airborne early warning (AEW) aircraft. Distinctive 24-foot rotodome atop the fuselage. The 'eyes of the fleet' — provides 360-degree surveillance for carrier strike groups.",
            specs = "Crew: 5 | Cruise: 576km/h | Endurance: 6h | Engines: 2x T56-A-427A",
            photoAsset = "aircraft/E2C.jpg",
            icaoTypeCodes = listOf("E2C")
        ),
        AircraftReference(
            id = "e3",
            name = "Boeing E-3 Sentry (AWACS)",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Airborne Warning and Control System based on Boeing 707 airframe. 30-foot rotating radar dome. Provides airborne command and control for air battle management. NATO backbone.",
            specs = "Crew: 19-34 | Cruise: 855km/h | Endurance: 8h | Engines: 4x TF33-PW-100A",
            photoAsset = "aircraft/E3CF.jpg",
            icaoTypeCodes = listOf("E3CF")
        ),
        AircraftReference(
            id = "e6b",
            name = "Boeing E-6B Mercury",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Airborne command post for US nuclear forces. Based on Boeing 707/KC-135 airframe. Trails a 5-mile-long antenna to communicate with ballistic missile submarines. Known as 'TACAMO.'",
            specs = "Crew: 22 | Cruise: 842km/h | Endurance: 15.5h | Engines: 4x CFM56-2A-2",
            photoAsset = "aircraft/E6B.jpg",
            icaoTypeCodes = listOf("E6B")
        ),
        AircraftReference(
            id = "kc10",
            name = "McDonnell Douglas KC-10 Extender",
            manufacturer = "McDonnell Douglas",
            category = AircraftCategory.FIGHTER,
            description = "Air refueling tanker based on DC-10 airframe. Can carry cargo and passengers simultaneously with fuel. Being retired and replaced by KC-46.",
            specs = "Crew: 4 | Fuel capacity: 161,508kg | Range: 18,507km | Engines: 3x CF6-50C2",
            photoAsset = "aircraft/KC10.jpg",
            icaoTypeCodes = listOf("KC10")
        ),
        AircraftReference(
            id = "kc46",
            name = "Boeing KC-46 Pegasus",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Next-generation tanker based on Boeing 767-200ER. Replaces aging KC-10 and KC-135 fleet. Features Remote Vision System for boom operations. Can refuel all NATO aircraft.",
            specs = "Crew: 3 | Fuel capacity: 96,098kg | Range: 12,200km | Engines: 2x PW4062",
            photoAsset = "aircraft/KC46.jpg",
            icaoTypeCodes = listOf("KC46")
        ),
        AircraftReference(
            id = "kc135",
            name = "Boeing KC-135 Stratotanker",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Venerable aerial refueling tanker in service since 1957. Based on Boeing 367-80 (707 prototype). Over 800 built. Being replaced by KC-46 but still backbone of USAF tanker fleet.",
            specs = "Crew: 3-4 | Fuel capacity: 92,210kg | Range: 2,419km | Engines: 4x CFM56-2B-1",
            photoAsset = "aircraft/KC135.jpg",
            icaoTypeCodes = listOf("KC135")
        ),
        AircraftReference(
            id = "p8",
            name = "Boeing P-8 Poseidon",
            manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "Maritime patrol and anti-submarine warfare aircraft based on Boeing 737-800. Replaced the P-3 Orion. Drops sonobuoys, torpedoes, and mines. Used by US Navy, Australia, India, and others.",
            specs = "Crew: 9 | Cruise: Mach 0.79 | Range: 7,242km | Engines: 2x CFM56-7B27A",
            photoAsset = "aircraft/P8.jpg",
            icaoTypeCodes = listOf("P8", "P8A")
        ),
        AircraftReference(
            id = "mq9",
            name = "General Atomics MQ-9 Reaper",
            manufacturer = "General Atomics",
            category = AircraftCategory.FIGHTER,
            description = "Primary US military hunter-killer drone. Carries Hellfire missiles, JDAM bombs, and surveillance pods. Crew operates remotely from ground stations thousands of miles away.",
            specs = "Wingspan: 20.1m | Max speed: 482km/h | Endurance: 27h | Engine: 1x TPE331-10GD",
            photoAsset = "aircraft/MQ9.jpg",
            icaoTypeCodes = listOf("MQ9")
        ),
        AircraftReference(
            id = "ac130",
            name = "Lockheed AC-130 Spectre/Ghostrider",
            manufacturer = "Lockheed Martin",
            category = AircraftCategory.FIGHTER,
            description = "Heavily armed gunship based on C-130 Hercules. Carries 30mm GAU-23 autocannon, 105mm howitzer, Griffin missiles. Orbits targets in a pylon turn delivering devastating firepower at night.",
            specs = "Crew: 13 | Cruise: 480km/h | Range: 4,630km | Engines: 4x T56-A-15",
            photoAsset = "aircraft/AC130.jpg",
            icaoTypeCodes = listOf("AC130")
        ),
        AircraftReference(
            id = "u2",
            name = "Lockheed U-2 Dragon Lady",
            manufacturer = "Lockheed (Skunk Works)",
            category = AircraftCategory.FIGHTER,
            description = "High-altitude reconnaissance aircraft flying at 70,000+ feet since 1955. Glider-like wings span 31m. Still in active service, now with digital sensors. Famous for Gary Powers shootdown over USSR.",
            specs = "Crew: 1 | Cruise: 805km/h | Ceiling: 70,000ft | Engine: 1x F118-GE-101",
            photoAsset = "aircraft/U2.jpg",
            icaoTypeCodes = listOf("U2")
        ),
        AircraftReference(
            id = "sr71",
            name = "Lockheed SR-71 Blackbird",
            manufacturer = "Lockheed (Skunk Works)",
            category = AircraftCategory.FIGHTER,
            description = "Fastest manned air-breathing aircraft ever built — Mach 3.3+. Titanium airframe that expands at speed. Retired in 1998. No aircraft has ever shot one down; it simply outran missiles.",
            specs = "Crew: 2 | Max speed: Mach 3.3 | Range: 5,400km | Engines: 2x J58-P-4",
            photoAsset = "aircraft/SR71.jpg",
            icaoTypeCodes = listOf("SR71")
        ),
        AircraftReference(
            id = "c12",
            name = "Beechcraft C-12 Huron",
            manufacturer = "Beechcraft/Textron",
            category = AircraftCategory.FIGHTER,
            description = "Military version of the King Air 200/350. Used for transport, ISR, and training by US Army, Navy, and Air Force. Over 360 in service. Multiple sensor and communications configurations.",
            specs = "Passengers: 8 | Range: 3,185km | Cruise: 536km/h | Engines: 2x PT6A-42",
            photoAsset = "aircraft/C12.jpg",
            icaoTypeCodes = listOf("C12")
        ),

        // ── Russian Fighters ──
        AircraftReference(
            id = "su27",
            name = "Sukhoi Su-27 Flanker",
            manufacturer = "Sukhoi (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Russia's premier air superiority fighter and one of the most agile jets ever built. Twin-engine, twin-tail design with exceptional range. Basis for an entire family of derivatives (Su-30, Su-33, Su-34, Su-35).",
            specs = "Crew: 1 | Max speed: Mach 2.35 | Range: 3,530km | Engines: 2x AL-31F",
            photoAsset = "aircraft/SU27.jpg",
            icaoTypeCodes = listOf("SU27")
        ),
        AircraftReference(
            id = "su30",
            name = "Sukhoi Su-30 Flanker-C",
            manufacturer = "Sukhoi (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Twin-seat multirole derivative of Su-27. Widely exported to India (Su-30MKI), China, Vietnam, Algeria, and others. Canard foreplanes on some variants for enhanced maneuverability.",
            specs = "Crew: 2 | Max speed: Mach 2.0 | Range: 3,000km | Engines: 2x AL-31FP",
            photoAsset = "aircraft/SU30.jpg",
            icaoTypeCodes = listOf("SU30")
        ),
        AircraftReference(
            id = "su34",
            name = "Sukhoi Su-34 Fullback",
            manufacturer = "Sukhoi (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Side-by-side two-seat fighter-bomber with distinctive 'platypus' nose. Armored cockpit, internal toilet, and galley for long missions. Heavily used for ground attack in Ukraine and Syria.",
            specs = "Crew: 2 | Max speed: Mach 1.8 | Range: 4,000km | Engines: 2x AL-31FM1",
            photoAsset = "aircraft/SU34.jpg",
            icaoTypeCodes = listOf("SU34")
        ),
        AircraftReference(
            id = "su35",
            name = "Sukhoi Su-35 Flanker-E",
            manufacturer = "Sukhoi (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Most advanced Flanker variant, designated 4++ generation. Thrust-vectoring engines enable extreme post-stall maneuvers. Irbis-E PESA radar. Exported to China, Egypt, and Indonesia.",
            specs = "Crew: 1 | Max speed: Mach 2.25 | Range: 3,600km | Engines: 2x AL-41F1S (117S)",
            photoAsset = "aircraft/SU35.jpg",
            icaoTypeCodes = listOf("SU35")
        ),
        AircraftReference(
            id = "mig29",
            name = "Mikoyan MiG-29 Fulcrum",
            manufacturer = "Mikoyan (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Lightweight frontal fighter designed to counter the F-16. Twin engines with distinctive wedge intakes. Widely exported to 30+ countries. Ukrainian MiG-29s have been adapted to fire Western weapons.",
            specs = "Crew: 1 | Max speed: Mach 2.25 | Range: 1,430km | Engines: 2x RD-33",
            photoAsset = "aircraft/MIG29.jpg",
            icaoTypeCodes = listOf("MIG29")
        ),
        AircraftReference(
            id = "mig31",
            name = "Mikoyan MiG-31 Foxhound",
            manufacturer = "Mikoyan (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "High-altitude, high-speed interceptor derived from MiG-25. Mach 2.83 capable with Zaslon PESA radar — first fighter with phased-array radar. Carries Kinzhal hypersonic missile on MiG-31K variant.",
            specs = "Crew: 2 | Max speed: Mach 2.83 | Range: 3,000km | Engines: 2x D-30F6",
            photoAsset = "aircraft/MIG31.jpg",
            icaoTypeCodes = listOf("MIG31")
        ),

        // ── Russian Bombers ──
        AircraftReference(
            id = "tu95",
            name = "Tupolev Tu-95 Bear",
            manufacturer = "Tupolev (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Nuclear-capable turboprop strategic bomber with distinctive contra-rotating propellers — the fastest propeller-driven aircraft. Routinely probes NATO airspace. Over 60 years in service.",
            specs = "Crew: 7 | Max speed: 925km/h | Range: 15,000km | Engines: 4x NK-12",
            photoAsset = "aircraft/TU95.jpg",
            icaoTypeCodes = listOf("TU95")
        ),
        AircraftReference(
            id = "tu160",
            name = "Tupolev Tu-160 Blackjack",
            manufacturer = "Tupolev (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "World's largest and heaviest combat aircraft. Supersonic swing-wing strategic bomber. Carries Kh-101/102 cruise missiles. New Tu-160M2 production restarted. Elegant white paint scheme.",
            specs = "Crew: 4 | Max speed: Mach 2.05 | Range: 12,300km | Engines: 4x NK-32",
            photoAsset = "aircraft/TU160.jpg",
            icaoTypeCodes = listOf("TU160")
        ),
        AircraftReference(
            id = "tu22m",
            name = "Tupolev Tu-22M Backfire",
            manufacturer = "Tupolev (Russia)",
            category = AircraftCategory.FIGHTER,
            description = "Supersonic swing-wing bomber designed for maritime strike and theater nuclear delivery. Carries Kh-22 anti-ship missiles. Used for strategic bombing in Ukraine and Syria.",
            specs = "Crew: 4 | Max speed: Mach 1.88 | Range: 6,800km | Engines: 2x NK-25",
            photoAsset = "aircraft/TU22.jpg",
            icaoTypeCodes = listOf("TU22")
        ),

        // ── Chinese Fighters ──
        AircraftReference(
            id = "j10",
            name = "Chengdu J-10 Vigorous Dragon",
            manufacturer = "Chengdu (China)",
            category = AircraftCategory.FIGHTER,
            description = "Chinese single-engine canard-delta multirole fighter. Comparable to F-16. J-10C variant features AESA radar and PL-15 long-range missiles. Backbone of PLAAF alongside J-11/J-16.",
            specs = "Crew: 1 | Max speed: Mach 2.2 | Range: 1,850km | Engine: 1x AL-31FN/WS-10B",
            photoAsset = "aircraft/J10.jpg",
            icaoTypeCodes = listOf("J10")
        ),
        AircraftReference(
            id = "j20",
            name = "Chengdu J-20 Mighty Dragon",
            manufacturer = "Chengdu (China)",
            category = AircraftCategory.FIGHTER,
            description = "China's first stealth fifth-generation fighter. Large canard-delta design optimized for long range and sensor fusion. WS-15 engine in development for supercruise. Over 200 believed in service.",
            specs = "Crew: 1 | Max speed: Mach 2.0+ | Range: 2,000km (est.) | Engines: 2x WS-10C/WS-15",
            photoAsset = "aircraft/J20.jpg",
            icaoTypeCodes = listOf("J20")
        ),
        AircraftReference(
            id = "jf17",
            name = "CAC/PAC JF-17 Thunder",
            manufacturer = "Chengdu/PAC (China/Pakistan)",
            category = AircraftCategory.FIGHTER,
            description = "Sino-Pakistani lightweight single-engine multirole fighter. Affordable F-16 alternative for developing nations. Block 3 variant adds AESA radar and HMD. Exported to Myanmar and Nigeria.",
            specs = "Crew: 1 | Max speed: Mach 1.6 | Range: 1,352km | Engine: 1x RD-93/WS-13",
            photoAsset = "aircraft/JF17.jpg",
            icaoTypeCodes = listOf("JF17")
        ),

        // ── European / International Fighters ──
        AircraftReference(
            id = "gripen",
            name = "Saab Gripen",
            manufacturer = "Saab (Sweden)",
            category = AircraftCategory.FIGHTER,
            description = "Swedish lightweight single-engine canard-delta fighter designed for dispersed road-base operations. Can be refueled and rearmed by conscript crew in 10 minutes. NATO-compatible. Gripen E/F is the latest variant.",
            specs = "Crew: 1-2 | Max speed: Mach 2.0 | Range: 3,200km | Engine: 1x RM12/F414G",
            photoAsset = "aircraft/GRF4.jpg",
            icaoTypeCodes = listOf("GRF4")
        ),
        AircraftReference(
            id = "tornado",
            name = "Panavia Tornado",
            manufacturer = "Panavia (UK/Germany/Italy)",
            category = AircraftCategory.FIGHTER,
            description = "Multinational swing-wing strike and interceptor aircraft. IDS variant for ground attack, ADV for air defense. Served in Gulf War, Kosovo, Libya, and Syria. Being retired in favor of Eurofighter.",
            specs = "Crew: 2 | Max speed: Mach 2.2 | Range: 3,890km | Engines: 2x RB199",
            photoAsset = "aircraft/GR4.jpg",
            icaoTypeCodes = listOf("GR4")
        ),
        AircraftReference(
            id = "mirage2000",
            name = "Dassault Mirage 2000",
            manufacturer = "Dassault (France)",
            category = AircraftCategory.FIGHTER,
            description = "French single-engine delta-wing multirole fighter. Widely exported to India, UAE, Greece, Taiwan, Egypt, and others. Mirage 2000D/N variants carry nuclear weapons for France. Predecessor to Rafale.",
            specs = "Crew: 1-2 | Max speed: Mach 2.2 | Range: 1,550km | Engine: 1x M53-P2",
            photoAsset = "aircraft/MIR2.jpg",
            icaoTypeCodes = listOf("MIRA", "MIR2")
        ),
        AircraftReference(
            id = "kfir",
            name = "IAI Kfir",
            manufacturer = "Israel Aerospace Industries",
            category = AircraftCategory.FIGHTER,
            description = "Israeli fighter derived from the Dassault Mirage III/5 with a GE J79 engine. Canard foreplanes added on C.2 variant. Used by Israel, Colombia, Ecuador, and Sri Lanka. Some converted to drones.",
            specs = "Crew: 1 | Max speed: Mach 2.3 | Range: 768km | Engine: 1x J79-GEJ1E",
            photoAsset = "aircraft/KFIR.jpg",
            icaoTypeCodes = listOf("KFIR")
        ),
        AircraftReference(
            id = "f14",
            name = "Grumman F-14 Tomcat",
            manufacturer = "Grumman",
            category = AircraftCategory.FIGHTER,
            description = "Iconic variable-sweep wing naval fighter made famous by 'Top Gun.' AIM-54 Phoenix missile could engage targets at 190km. Retired from US Navy in 2006 but still flown by Iran's air force.",
            specs = "Crew: 2 | Max speed: Mach 2.34 | Range: 2,960km | Engines: 2x TF30/F110-GE-400",
            photoAsset = "aircraft/F14.jpg",
            icaoTypeCodes = listOf("F14")
        ),

        // ══════════════════════════════════════
        // ── Trainers ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "t6",
            name = "Beechcraft T-6 Texan II",
            manufacturer = "Beechcraft/Textron",
            category = AircraftCategory.TRAINER,
            description = "Primary trainer for USAF and USN pilots. Turboprop single-engine, tandem cockpit. All US military pilots learn to fly in the T-6. Based on Pilatus PC-9.",
            specs = "Crew: 2 | Max speed: 585km/h | Range: 1,667km | Engine: 1x PT6A-68",
            photoAsset = "aircraft/T6.jpg",
            icaoTypeCodes = listOf("T6")
        ),
        AircraftReference(
            id = "t38",
            name = "Northrop T-38 Talon",
            manufacturer = "Northrop",
            category = AircraftCategory.TRAINER,
            description = "Supersonic twin-engine jet trainer in service since 1961. First supersonic trainer and still used by USAF for advanced training. NASA astronauts also fly T-38s. Being replaced by T-7A.",
            specs = "Crew: 2 | Max speed: Mach 1.23 | Range: 1,835km | Engines: 2x J85-GE-5A",
            photoAsset = "aircraft/T38.jpg",
            icaoTypeCodes = listOf("T38")
        ),
        AircraftReference(
            id = "t45",
            name = "Boeing T-45 Goshawk",
            manufacturer = "Boeing/BAE",
            category = AircraftCategory.TRAINER,
            description = "US Navy's carrier-capable jet trainer based on BAE Hawk. Used for carrier landing qualification. Students learn carrier approach and arrested landings. Single GE Adour engine.",
            specs = "Crew: 2 | Max speed: Mach 0.90 | Range: 1,480km | Engine: 1x Adour Mk871",
            photoAsset = "aircraft/T45.jpg",
            icaoTypeCodes = listOf("T45")
        ),

        // ══════════════════════════════════════
        // ── Cargo / Transport ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "c130",
            name = "Lockheed C-130 Hercules/Super Hercules",
            manufacturer = "Lockheed Martin",
            category = AircraftCategory.CARGO,
            description = "Most versatile military transport ever built. In production since 1954 with over 2,600 delivered. Used for cargo, paradrop, aerial refueling, firefighting, weather recon, and gunship. C-130J is the current production variant.",
            specs = "Payload: 20,400kg | Range: 3,800km | Cruise: 643km/h | Engines: 4x T56/AE2100",
            photoAsset = "aircraft/C130.jpg",
            icaoTypeCodes = listOf("C130", "C130H", "C30J")
        ),
        AircraftReference(
            id = "c17",
            name = "Boeing C-17 Globemaster III",
            manufacturer = "Boeing",
            category = AircraftCategory.CARGO,
            description = "Heavy strategic/tactical airlifter that can carry 77 tons and land on short/austere runways. Externally blown flap system enables steep approaches. 279 built for US and allies.",
            specs = "Payload: 77,519kg | Range: 4,482km | Cruise: 833km/h | Engines: 4x F117-PW-100",
            photoAsset = "aircraft/C17.jpg",
            icaoTypeCodes = listOf("C17", "C17A")
        ),
        AircraftReference(
            id = "c5",
            name = "Lockheed C-5 Galaxy",
            manufacturer = "Lockheed Martin",
            category = AircraftCategory.CARGO,
            description = "One of the largest military aircraft in the world. Front and rear cargo doors allow drive-through loading. Can carry two M1 Abrams tanks. C-5M Super Galaxy has new GE CF6 engines.",
            specs = "Payload: 129,274kg | Range: 4,440km | Cruise: 833km/h | Engines: 4x GE CF6-80C2",
            photoAsset = "aircraft/C5.jpg",
            icaoTypeCodes = listOf("C5", "C5M")
        ),
        AircraftReference(
            id = "a400m",
            name = "Airbus A400M Atlas",
            manufacturer = "Airbus",
            category = AircraftCategory.CARGO,
            description = "European four-engine turboprop tactical/strategic transport. Fills gap between C-130 and C-17. Contra-rotating propellers reduce torque effects. Used by 8 European nations and Malaysia.",
            specs = "Payload: 37,000kg | Range: 4,540km | Cruise: 780km/h | Engines: 4x TP400-D6",
            photoAsset = "aircraft/A400.jpg",
            icaoTypeCodes = listOf("A400", "A400M")
        ),
        AircraftReference(
            id = "il76",
            name = "Ilyushin Il-76",
            manufacturer = "Ilyushin (Russia)",
            category = AircraftCategory.CARGO,
            description = "Soviet-era four-engine strategic airlifter. Rear cargo ramp and high wing. Still in production as Il-76MD-90A. Used by Russian Air Force and civilian cargo operators worldwide.",
            specs = "Payload: 50,000kg | Range: 4,400km | Cruise: 800km/h | Engines: 4x D-30KP/PS-90A",
            photoAsset = "aircraft/IL76.jpg",
            icaoTypeCodes = listOf("IL76")
        ),
        AircraftReference(
            id = "an124",
            name = "Antonov An-124 Ruslan",
            manufacturer = "Antonov (Ukraine)",
            category = AircraftCategory.CARGO,
            description = "World's largest serially produced cargo aircraft. Kneeling nose gear and front cargo door. Used for oversize cargo worldwide. An-124s have carried satellites, locomotives, and aircraft fuselages.",
            specs = "Payload: 150,000kg | Range: 4,800km | Cruise: 865km/h | Engines: 4x D-18T",
            photoAsset = "aircraft/AN124.jpg",
            icaoTypeCodes = listOf("AN124")
        ),
        AircraftReference(
            id = "c295",
            name = "Airbus C-295",
            manufacturer = "Airbus",
            category = AircraftCategory.CARGO,
            description = "Light tactical transport and maritime patrol aircraft. Twin PW127 turboprops. Rear ramp for cargo and paradrop. Used by 35+ countries for transport, SAR, and MPA roles.",
            specs = "Payload: 9,250kg | Range: 5,630km | Cruise: 480km/h | Engines: 2x PW127G",
            photoAsset = "aircraft/C295.jpg",
            icaoTypeCodes = listOf("C295", "C295W")
        ),

        // ══════════════════════════════════════
        // ── Light Aircraft ──
        // ══════════════════════════════════════

        AircraftReference(
            id = "c172",
            name = "Cessna 172 Skyhawk",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.LIGHTPLANE,
            description = "Most produced aircraft in history with 45,000+ built. High-wing four-seater, the definitive training aircraft. If you learn to fly, it's probably in a 172. Still in production.",
            specs = "Passengers: 3 | Range: 1,289km | Cruise: 226km/h | Engine: 1x Lycoming IO-360",
            photoAsset = "aircraft/C172.jpg",
            icaoTypeCodes = listOf("C172")
        ),
        AircraftReference(
            id = "c182",
            name = "Cessna 182 Skylane",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.LIGHTPLANE,
            description = "Step-up from the 172 with more power and useful load. High-wing four-seater popular for personal transport and aerial survey. 23,000+ built. Reliable and forgiving.",
            specs = "Passengers: 3 | Range: 1,520km | Cruise: 267km/h | Engine: 1x Lycoming IO-540",
            photoAsset = "aircraft/C182.jpg",
            icaoTypeCodes = listOf("C182")
        ),
        AircraftReference(
            id = "c152",
            name = "Cessna 152",
            manufacturer = "Cessna",
            category = AircraftCategory.LIGHTPLANE,
            description = "Two-seat training aircraft produced 1977-1985. Simplified, economical trainer. Over 7,500 built. Many still active at flight schools worldwide.",
            specs = "Passengers: 1 | Range: 768km | Cruise: 198km/h | Engine: 1x Lycoming O-235",
            photoAsset = "aircraft/C152.jpg",
            icaoTypeCodes = listOf("C152")
        ),
        AircraftReference(
            id = "c150",
            name = "Cessna 150",
            manufacturer = "Cessna",
            category = AircraftCategory.LIGHTPLANE,
            description = "Predecessor to the Cessna 152. Over 23,800 built (1959-1977), making it second most-produced Cessna. Tricycle gear trainer that taught generations of pilots.",
            specs = "Passengers: 1 | Range: 738km | Cruise: 195km/h | Engine: 1x Continental O-200",
            photoAsset = "aircraft/C150.jpg",
            icaoTypeCodes = listOf("C150")
        ),
        AircraftReference(
            id = "pa28",
            name = "Piper Cherokee/Warrior/Archer",
            manufacturer = "Piper",
            category = AircraftCategory.LIGHTPLANE,
            description = "Low-wing four-seat trainer/tourer. Second most popular training aircraft after the Cessna 172. Multiple variants from the 140hp Cherokee to 180hp Archer III. Tapered Hershey-bar wings.",
            specs = "Passengers: 3 | Range: 1,148km | Cruise: 228km/h | Engine: 1x Lycoming O-360",
            photoAsset = "aircraft/PA28.jpg",
            icaoTypeCodes = listOf("P28A", "PA28")
        ),
        AircraftReference(
            id = "pa32",
            name = "Piper Cherokee Six/Saratoga/Lance",
            manufacturer = "Piper",
            category = AircraftCategory.LIGHTPLANE,
            description = "Six-seat single-engine Piper. Larger fuselage than Cherokee with more payload. Retractable gear versions (Lance/Saratoga) offer higher speed. Popular for family transport.",
            specs = "Passengers: 5 | Range: 1,380km | Cruise: 277km/h | Engine: 1x Lycoming IO-540",
            photoAsset = "aircraft/PA32.jpg",
            icaoTypeCodes = listOf("PA32")
        ),
        AircraftReference(
            id = "c210",
            name = "Cessna 210 Centurion",
            manufacturer = "Cessna",
            category = AircraftCategory.LIGHTPLANE,
            description = "High-performance retractable-gear single. The only high-wing retractable Cessna. Popular for cross-country trips with 6-seat cabin. Turbo versions reach 20,000+ ft.",
            specs = "Passengers: 5 | Range: 1,720km | Cruise: 298km/h | Engine: 1x Continental IO-520",
            photoAsset = "aircraft/C210.jpg",
            icaoTypeCodes = listOf("C210")
        ),
        AircraftReference(
            id = "c206",
            name = "Cessna 206 Stationair",
            manufacturer = "Cessna/Textron",
            category = AircraftCategory.LIGHTPLANE,
            description = "Workhorse utility single-engine. Large cargo door and 1,635lb useful load. Flies from floats, wheels, and skis. Popular for bush flying, skydiving, and missionary aviation.",
            specs = "Passengers: 5 | Range: 1,352km | Cruise: 266km/h | Engine: 1x Lycoming IO-540",
            photoAsset = "aircraft/C206.jpg",
            icaoTypeCodes = listOf("C206")
        ),
        AircraftReference(
            id = "bonanza",
            name = "Beechcraft Bonanza",
            manufacturer = "Beechcraft/Textron",
            category = AircraftCategory.LIGHTPLANE,
            description = "Longest continuously produced aircraft (since 1947). High-performance retractable single. Classic V-tail Bonanza is iconic; later A36 has conventional tail. The 'Cadillac of the sky.'",
            specs = "Passengers: 5 | Range: 1,667km | Cruise: 326km/h | Engine: 1x Continental IO-550",
            photoAsset = "aircraft/BE36.jpg",
            icaoTypeCodes = listOf("BE36")
        ),
        AircraftReference(
            id = "baron",
            name = "Beechcraft Baron",
            manufacturer = "Beechcraft/Textron",
            category = AircraftCategory.LIGHTPLANE,
            description = "Light twin-engine aircraft. Evolved from the Twin Bonanza and Travel Air. Popular for multi-engine training and personal transportation. Baron 58 is the six-seat version.",
            specs = "Passengers: 5 | Range: 2,020km | Cruise: 373km/h | Engines: 2x Continental IO-550",
            photoAsset = "aircraft/BE58.jpg",
            icaoTypeCodes = listOf("BE55", "BE58")
        ),
        AircraftReference(
            id = "da40",
            name = "Diamond DA40 Diamond Star",
            manufacturer = "Diamond Aircraft",
            category = AircraftCategory.LIGHTPLANE,
            description = "Austrian composite four-seater. T-tail, low wing, and sleek lines. Popular at flight schools for its Garmin G1000 glass cockpit and docile handling. Diesel/Jet-A variant available.",
            specs = "Passengers: 3 | Range: 1,341km | Cruise: 256km/h | Engine: 1x Lycoming IO-360",
            photoAsset = "aircraft/DA40.jpg",
            icaoTypeCodes = listOf("DA40")
        ),
        AircraftReference(
            id = "da42",
            name = "Diamond DA42 Twin Star",
            manufacturer = "Diamond Aircraft",
            category = AircraftCategory.LIGHTPLANE,
            description = "Light twin with diesel/Jet-A engines. All-composite airframe. Popular for multi-engine training and surveillance. FADEC engine management simplifies operations.",
            specs = "Passengers: 3 | Range: 1,693km | Cruise: 326km/h | Engines: 2x Austro AE300",
            photoAsset = "aircraft/DA42.jpg",
            icaoTypeCodes = listOf("DA42")
        ),
        AircraftReference(
            id = "sr22",
            name = "Cirrus SR22/SR22T",
            manufacturer = "Cirrus Aircraft",
            category = AircraftCategory.LIGHTPLANE,
            description = "Best-selling single-engine piston aircraft of the 21st century. Famous for the Cirrus Airframe Parachute System (CAPS) — a whole-aircraft parachute. Side-stick, composite, glass cockpit.",
            specs = "Passengers: 4 | Range: 1,685km | Cruise: 338km/h | Engine: 1x Continental IO-550/TSIO-550",
            photoAsset = "aircraft/SR22.jpg",
            icaoTypeCodes = listOf("SR22")
        ),
        AircraftReference(
            id = "sr20",
            name = "Cirrus SR20",
            manufacturer = "Cirrus Aircraft",
            category = AircraftCategory.LIGHTPLANE,
            description = "Entry-level Cirrus with CAPS parachute. Composite airframe, side-stick control. Popular for training and personal use. First GA aircraft with whole-aircraft parachute from the factory.",
            specs = "Passengers: 4 | Range: 1,204km | Cruise: 286km/h | Engine: 1x Continental IO-360-ES",
            photoAsset = "aircraft/SR20.jpg",
            icaoTypeCodes = listOf("SR20")
        ),
        AircraftReference(
            id = "mooney",
            name = "Mooney M20 (Ovation/Acclaim)",
            manufacturer = "Mooney",
            category = AircraftCategory.LIGHTPLANE,
            description = "Speed-optimized four-seater with distinctive forward-swept vertical tail. Known as the fastest single-engine piston aircraft. Manual gear and prop control. Cult following among speed enthusiasts.",
            specs = "Passengers: 3 | Range: 2,105km | Cruise: 362km/h | Engine: 1x Continental TSIO-550-G",
            photoAsset = "aircraft/M20P.jpg",
            icaoTypeCodes = listOf("M20P", "M20T")
        ),
        AircraftReference(
            id = "pa18",
            name = "Piper PA-18 Super Cub",
            manufacturer = "Piper",
            category = AircraftCategory.LIGHTPLANE,
            description = "Classic taildragger bush plane. Tandem seating with excellent visibility. Over 9,000 built (1949-1994). Still highly prized for backcountry flying, towing gliders, and ag work.",
            specs = "Passengers: 1 | Range: 740km | Cruise: 185km/h | Engine: 1x Lycoming O-320",
            photoAsset = "aircraft/PA18.jpg",
            icaoTypeCodes = listOf("PA18")
        ),
        AircraftReference(
            id = "be76",
            name = "Beechcraft Duchess",
            manufacturer = "Beechcraft",
            category = AircraftCategory.LIGHTPLANE,
            description = "Light twin designed specifically for multi-engine training. T-tail design. Produced 1978-1982 with 437 built. Common at flight schools for commercial multi-engine rating.",
            specs = "Passengers: 3 | Range: 1,370km | Cruise: 306km/h | Engines: 2x Lycoming O-360",
            photoAsset = "aircraft/BE76.jpg",
            icaoTypeCodes = listOf("BE76")
        ),
        AircraftReference(
            id = "pa44",
            name = "Piper Seminole",
            manufacturer = "Piper",
            category = AircraftCategory.LIGHTPLANE,
            description = "Light twin-engine trainer based on Cherokee airframe. Counter-rotating props eliminate critical engine factor. Standard multi-engine training aircraft at many flight schools.",
            specs = "Passengers: 3 | Range: 1,500km | Cruise: 311km/h | Engines: 2x Lycoming O-360",
            photoAsset = "aircraft/PA44.jpg",
            icaoTypeCodes = listOf("PA44")
        ),
        AircraftReference(
            id = "pa34",
            name = "Piper Seneca",
            manufacturer = "Piper",
            category = AircraftCategory.LIGHTPLANE,
            description = "Six-seat light twin for personal and charter use. Turbo versions offer good altitude performance. Retractable gear, counter-rotating props on later models. Popular IFR platform.",
            specs = "Passengers: 5 | Range: 1,490km | Cruise: 361km/h | Engines: 2x Continental TSIO-360",
            photoAsset = "aircraft/PA34.jpg",
            icaoTypeCodes = listOf("PA34")
        ),
        AircraftReference(
            id = "c310",
            name = "Cessna 310",
            manufacturer = "Cessna",
            category = AircraftCategory.LIGHTPLANE,
            description = "Classic light twin with tip tanks and distinctive swept tail. First twin produced by Cessna. Over 6,300 built. Recognizable in pop culture as the airplane in TV's 'Sky King.'",
            specs = "Passengers: 5 | Range: 2,480km | Cruise: 335km/h | Engines: 2x Continental IO-520",
            photoAsset = "aircraft/C310.jpg",
            icaoTypeCodes = listOf("C310")
        ),
        AircraftReference(
            id = "c340",
            name = "Cessna 340/340A",
            manufacturer = "Cessna",
            category = AircraftCategory.LIGHTPLANE,
            description = "Pressurized light twin. The smallest pressurized twin Cessna made. Six seats, turbocharged Continental engines. Cruise comfortably above weather at FL200.",
            specs = "Passengers: 5 | Range: 2,575km | Cruise: 394km/h | Engines: 2x Continental TSIO-520",
            photoAsset = "aircraft/C340.jpg",
            icaoTypeCodes = listOf("C340")
        ),
        AircraftReference(
            id = "c414",
            name = "Cessna 414/421 Chancellor/Golden Eagle",
            manufacturer = "Cessna",
            category = AircraftCategory.LIGHTPLANE,
            description = "Pressurized cabin-class twins. The 421 Golden Eagle is the larger, more powerful variant with geared Continental GTSIO-520 engines. Used for charter, corporate, and personal transport.",
            specs = "Passengers: 5-7 | Range: 2,580km | Cruise: 396km/h | Engines: 2x Continental TSIO/GTSIO-520",
            photoAsset = "aircraft/C421.jpg",
            icaoTypeCodes = listOf("C414", "C421")
        ),
        // === NEW ENTRIES: Round 4 enrichment (35 aircraft) ===
        // Commercial variants
        AircraftReference(
            id = "b736", name = "Boeing 737-600", manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "The smallest variant of the 737 Next Generation family, introduced to replace the 737-500 with modernized avionics and an upgraded wing design.",
            specs = "Crew: 2 | Range: 5,990km | Cruise: 828km/h | Engines: 2x CFM56-7B turbofans",
            photoAsset = "aircraft/B737.jpg", icaoTypeCodes = listOf("B736")
        ),
        AircraftReference(
            id = "b37m", name = "Boeing 737 MAX 7", manufacturer = "Boeing",
            category = AircraftCategory.NARROWBODY,
            description = "The smallest variant of the 737 MAX series offering improved fuel efficiency, greater range, and a quieter cabin compared to the 737-700.",
            specs = "Crew: 2 | Range: 7,130km | Cruise: 839km/h | Engines: 2x CFM LEAP-1B turbofans",
            photoAsset = "aircraft/B38M.jpg", icaoTypeCodes = listOf("B37M")
        ),
        AircraftReference(
            id = "b77f", name = "Boeing 777 Freighter", manufacturer = "Boeing",
            category = AircraftCategory.CARGO,
            description = "A dedicated twin-engine freighter based on the 777-200LR, renowned for immense payload capacity and long-range efficiency.",
            specs = "Crew: 2 | Range: 9,200km | Cruise: 896km/h | Engines: 2x GE90-110B1L turbofans",
            photoAsset = "aircraft/B77W.jpg", icaoTypeCodes = listOf("B77F")
        ),
        // Military fighters
        AircraftReference(
            id = "su57", name = "Sukhoi Su-57 Felon", manufacturer = "Sukhoi",
            category = AircraftCategory.FIGHTER,
            description = "Russia's stealth fifth-generation twin-engine multirole fighter for air superiority and ground strike missions.",
            specs = "Crew: 1 | Range: 3,500km | Cruise: Mach 1.3 | Engines: 2x Saturn AL-41F1",
            photoAsset = "aircraft/F22.jpg", icaoTypeCodes = listOf("SU57")
        ),
        AircraftReference(
            id = "su25", name = "Sukhoi Su-25 Frogfoot", manufacturer = "Sukhoi",
            category = AircraftCategory.FIGHTER,
            description = "A heavily armored Soviet close air support jet, exceptionally effective at low-altitude ground attack missions.",
            specs = "Crew: 1 | Range: 1,000km | Cruise: 750km/h | Engines: 2x R-195 turbojets",
            photoAsset = "aircraft/A10.jpg", icaoTypeCodes = listOf("SU25")
        ),
        AircraftReference(
            id = "fc31", name = "Shenyang FC-31 Gyrfalcon", manufacturer = "Shenyang Aircraft Corp",
            category = AircraftCategory.FIGHTER,
            description = "A Chinese fifth-generation twin-engine stealth fighter in advanced development for multirole combat and export.",
            specs = "Crew: 1 | Range: 1,200km | Cruise: Mach 1.8 | Engines: 2x WS-13E",
            photoAsset = "aircraft/F35.jpg", icaoTypeCodes = listOf("FC31")
        ),
        AircraftReference(
            id = "y20", name = "Xian Y-20 Kunpeng", manufacturer = "Xi'an Aircraft Industrial Corp",
            category = AircraftCategory.CARGO,
            description = "A massive Chinese military transport providing heavy airlift capability for rapid deployment and strategic logistics.",
            specs = "Crew: 3 | Range: 7,800km | Cruise: 800km/h | Engines: 4x D-30KP-2 or WS-20",
            photoAsset = "aircraft/C17.jpg", icaoTypeCodes = listOf("Y20")
        ),
        AircraftReference(
            id = "tu16", name = "Xian H-6", manufacturer = "Xi'an Aircraft Industrial Corp",
            category = AircraftCategory.FIGHTER,
            description = "A twin-engine jet bomber based on the Soviet Tu-16, extensively modernized as the backbone of China's strategic bomber force.",
            specs = "Crew: 4 | Range: 6,000km | Cruise: 768km/h | Engines: 2x D-30KP-2 turbofans",
            photoAsset = "aircraft/B52.jpg", icaoTypeCodes = listOf("TU16")
        ),
        AircraftReference(
            id = "wz10", name = "Changhe Z-10", manufacturer = "CAIC",
            category = AircraftCategory.HELICOPTER,
            description = "China's first dedicated modern attack helicopter, optimized for anti-tank warfare with tandem-seat cockpit and stealth features.",
            specs = "Crew: 2 | Range: 800km | Cruise: 270km/h | Engines: 2x WZ-9 turboshafts",
            photoAsset = "aircraft/AH64.jpg", icaoTypeCodes = listOf("WZ10")
        ),
        AircraftReference(
            id = "an72", name = "Antonov An-72", manufacturer = "Antonov",
            category = AircraftCategory.CARGO,
            description = "A rugged Soviet STOL transport utilizing the Coanda effect with engines mounted over the wings for exceptional short-field performance.",
            specs = "Crew: 3 | Range: 2,000km | Cruise: 600km/h | Engines: 2x Progress D-36 turbofans",
            photoAsset = "aircraft/C130.jpg", icaoTypeCodes = listOf("AN72")
        ),
        AircraftReference(
            id = "f4", name = "McDonnell Douglas F-4 Phantom II", manufacturer = "McDonnell Douglas",
            category = AircraftCategory.FIGHTER,
            description = "A legendary tandem two-seat supersonic jet interceptor that served prolifically during the Cold War and Vietnam War.",
            specs = "Crew: 2 | Range: 2,600km | Cruise: 940km/h | Engines: 2x GE J79 turbojets",
            photoAsset = "aircraft/F15.jpg", icaoTypeCodes = listOf("F4")
        ),
        AircraftReference(
            id = "f5", name = "Northrop F-5 Tiger II", manufacturer = "Northrop",
            category = AircraftCategory.FIGHTER,
            description = "An iconic light supersonic fighter renowned for aerodynamic simplicity, low cost, and effectiveness as an aggressor training aircraft.",
            specs = "Crew: 1 | Range: 1,400km | Cruise: 1,040km/h | Engines: 2x GE J85-GE-21B turbojets",
            photoAsset = "aircraft/F16.jpg", icaoTypeCodes = listOf("F5")
        ),
        AircraftReference(
            id = "kfir", name = "IAI Kfir", manufacturer = "Israel Aerospace Industries",
            category = AircraftCategory.FIGHTER,
            description = "An Israeli all-weather multirole fighter developed by mating the Dassault Mirage 5 airframe with a US-built J79 turbojet engine.",
            specs = "Crew: 1 | Range: 768km | Cruise: Mach 2.0 | Engines: 1x GE J79-J1E turbojet",
            photoAsset = "aircraft/RFAL.jpg", icaoTypeCodes = listOf("KFIR")
        ),
        // Military helicopters
        AircraftReference(
            id = "ch53", name = "Sikorsky CH-53 Sea Stallion", manufacturer = "Sikorsky",
            category = AircraftCategory.HELICOPTER,
            description = "A massive heavy-lift transport helicopter for the US Marine Corps, excelling at deploying heavy equipment and large troop contingents.",
            specs = "Crew: 3 | Range: 1,000km | Cruise: 278km/h | Engines: 2x GE T64-GE-413 turboshafts",
            photoAsset = "", icaoTypeCodes = listOf("CH53")
        ),
        AircraftReference(
            id = "ch46", name = "Boeing CH-46 Sea Knight", manufacturer = "Boeing Vertol",
            category = AircraftCategory.HELICOPTER,
            description = "A medium-lift tandem-rotor transport helicopter relied upon by the US Marine Corps for troop transport and ship-to-ship resupply.",
            specs = "Crew: 5 | Range: 1,020km | Cruise: 265km/h | Engines: 2x GE T58-GE-16 turboshafts",
            photoAsset = "aircraft/CH47.jpg", icaoTypeCodes = listOf("CH46")
        ),
        AircraftReference(
            id = "mh60", name = "Sikorsky MH-60 Seahawk", manufacturer = "Sikorsky",
            category = AircraftCategory.HELICOPTER,
            description = "A multi-mission US Navy helicopter based on the UH-60 Black Hawk, equipped for anti-submarine warfare and search and rescue.",
            specs = "Crew: 3-4 | Range: 834km | Cruise: 268km/h | Engines: 2x GE T700-GE-401C turboshafts",
            photoAsset = "aircraft/UH60.jpg", icaoTypeCodes = listOf("MH60")
        ),
        AircraftReference(
            id = "sh60", name = "Sikorsky SH-60 Seahawk", manufacturer = "Sikorsky",
            category = AircraftCategory.HELICOPTER,
            description = "A twin-turboshaft multi-mission US Navy helicopter serving as a ship-borne platform for anti-surface and anti-submarine operations.",
            specs = "Crew: 3 | Range: 834km | Cruise: 268km/h | Engines: 2x GE T700-GE-401C turboshafts",
            photoAsset = "aircraft/UH60.jpg", icaoTypeCodes = listOf("SH60")
        ),
        AircraftReference(
            id = "hh60", name = "Sikorsky HH-60 Pave Hawk", manufacturer = "Sikorsky",
            category = AircraftCategory.HELICOPTER,
            description = "A modified Black Hawk for combat search and rescue featuring an integrated rescue hoist and air-to-air refueling probe.",
            specs = "Crew: 4 | Range: 600km | Cruise: 294km/h | Engines: 2x GE T700-GE-700/701C turboshafts",
            photoAsset = "aircraft/UH60.jpg", icaoTypeCodes = listOf("HH60")
        ),
        AircraftReference(
            id = "ah1z", name = "Bell AH-1Z Viper", manufacturer = "Bell Helicopter",
            category = AircraftCategory.HELICOPTER,
            description = "A lethal twin-engine attack helicopter based on the AH-1W SuperCobra with a four-blade rotor and advanced targeting.",
            specs = "Crew: 2 | Range: 685km | Cruise: 296km/h | Engines: 2x GE T700-GE-401C turboshafts",
            photoAsset = "aircraft/AH64.jpg", icaoTypeCodes = listOf("AH1Z")
        ),
        AircraftReference(
            id = "nh90", name = "NHIndustries NH90", manufacturer = "NHIndustries",
            category = AircraftCategory.HELICOPTER,
            description = "A medium-sized twin-engine multi-role NATO helicopter — the world's first to fly with full fly-by-wire controls.",
            specs = "Crew: 2 | Range: 800km | Cruise: 300km/h | Engines: 2x RTM322 turboshafts",
            photoAsset = "aircraft/UH60.jpg", icaoTypeCodes = listOf("NH90")
        ),
        AircraftReference(
            id = "eh10", name = "AgustaWestland EH101 Merlin", manufacturer = "AgustaWestland",
            category = AircraftCategory.HELICOPTER,
            description = "A capable medium-lift helicopter with a distinctive three-engine configuration for enhanced safety and power in offshore operations.",
            specs = "Crew: 3-4 | Range: 833km | Cruise: 278km/h | Engines: 3x RTM322 turboshafts",
            photoAsset = "aircraft/S92.jpg", icaoTypeCodes = listOf("EH10")
        ),
        AircraftReference(
            id = "mi26", name = "Mil Mi-26 Halo", manufacturer = "Mil",
            category = AircraftCategory.HELICOPTER,
            description = "The largest and most powerful helicopter ever to enter serial production, capable of lifting up to 20 tons of cargo.",
            specs = "Crew: 5 | Range: 800km | Cruise: 255km/h | Engines: 2x Lotarev D-136 turboshafts",
            photoAsset = "aircraft/CH47.jpg", icaoTypeCodes = listOf("MI26")
        ),
        // UAVs
        AircraftReference(
            id = "mq1", name = "General Atomics MQ-1 Predator", manufacturer = "General Atomics",
            category = AircraftCategory.FIGHTER,
            description = "An iconic remotely piloted aircraft that ushered in the modern era of military drones, later armed for targeted strike missions.",
            specs = "Crew: 0 | Range: 1,100km | Cruise: 135km/h | Engines: 1x Rotax 914F turbocharged",
            photoAsset = "aircraft/MQ9.jpg", icaoTypeCodes = listOf("MQ1")
        ),
        AircraftReference(
            id = "rq4", name = "Northrop Grumman RQ-4 Global Hawk", manufacturer = "Northrop Grumman",
            category = AircraftCategory.FIGHTER,
            description = "A high-altitude, long-endurance unmanned surveillance platform capable of gathering broad-area intelligence over incredible distances.",
            specs = "Crew: 0 | Range: 22,780km | Cruise: 575km/h | Engines: 1x Rolls-Royce F137-RR-100 turbofan",
            photoAsset = "aircraft/MQ9.jpg", icaoTypeCodes = listOf("RQ4")
        ),
        // Business jets
        AircraftReference(
            id = "e550", name = "Embraer Praetor 500", manufacturer = "Embraer",
            category = AircraftCategory.BIZJET,
            description = "A premium mid-size business jet with coast-to-coast range, ultra-modern avionics, and refined cabin comfort.",
            specs = "Crew: 2 | Range: 6,186km | Cruise: 863km/h | Engines: 2x Honeywell HTF7500E turbofans",
            photoAsset = "aircraft/E55P.jpg", icaoTypeCodes = listOf("E550")
        ),
        AircraftReference(
            id = "pc24", name = "Pilatus PC-24", manufacturer = "Pilatus Aircraft",
            category = AircraftCategory.BIZJET,
            description = "The 'Super Versatile Jet' uniquely combining jet speed with the short, unpaved runway capability of a turboprop.",
            specs = "Crew: 1-2 | Range: 3,704km | Cruise: 815km/h | Engines: 2x Williams FJ44-4A turbofans",
            photoAsset = "aircraft/PC12.jpg", icaoTypeCodes = listOf("PC24")
        ),
        AircraftReference(
            id = "sf50", name = "Cirrus Vision Jet SF50", manufacturer = "Cirrus Aircraft",
            category = AircraftCategory.BIZJET,
            description = "A groundbreaking single-engine very light jet with a unique V-tail and whole-airframe parachute system for personal/owner-pilot use.",
            specs = "Crew: 1 | Range: 2,222km | Cruise: 556km/h | Engines: 1x Williams FJ33-5A turbofan",
            photoAsset = "aircraft/HDJT.jpg", icaoTypeCodes = listOf("SF50")
        ),
        // Civilian helicopters
        AircraftReference(
            id = "a169", name = "Leonardo AW169", manufacturer = "Leonardo S.p.A.",
            category = AircraftCategory.HELICOPTER,
            description = "A modern twin-engine light intermediate helicopter popular for VIP transport, EMS, and law enforcement.",
            specs = "Crew: 1-2 | Range: 820km | Cruise: 268km/h | Engines: 2x PW210A turboshafts",
            photoAsset = "aircraft/A139.jpg", icaoTypeCodes = listOf("A169")
        ),
        AircraftReference(
            id = "b505", name = "Bell 505 Jet Ranger X", manufacturer = "Bell Helicopter",
            category = AircraftCategory.HELICOPTER,
            description = "A modern light single-engine successor to the legendary Bell 206 JetRanger with advanced avionics and a flat floor.",
            specs = "Crew: 1 | Range: 566km | Cruise: 232km/h | Engines: 1x Safran Arrius 2R turboshaft",
            photoAsset = "aircraft/B407.jpg", icaoTypeCodes = listOf("B505")
        ),
        AircraftReference(
            id = "s70", name = "Sikorsky S-70 Black Hawk (Export)", manufacturer = "Sikorsky",
            category = AircraftCategory.HELICOPTER,
            description = "The export and commercial variant of the UH-60 Black Hawk, universally revered for extreme ruggedness and versatility.",
            specs = "Crew: 2 | Range: 592km | Cruise: 277km/h | Engines: 2x GE T700-GE-701C turboshafts",
            photoAsset = "aircraft/UH60.jpg", icaoTypeCodes = listOf("S70")
        ),
        AircraftReference(
            id = "ec55", name = "Airbus EC155 / AS365 Dauphin", manufacturer = "Airbus Helicopters",
            category = AircraftCategory.HELICOPTER,
            description = "A sleek medium twin-engine helicopter with a shrouded Fenestron tail rotor, popular for VIP transport and offshore operations.",
            specs = "Crew: 1-2 | Range: 857km | Cruise: 280km/h | Engines: 2x Arriel 2C2 turboshafts",
            photoAsset = "aircraft/EC45.jpg", icaoTypeCodes = listOf("EC55")
        ),
        AircraftReference(
            id = "as32", name = "Airbus AS332 Super Puma", manufacturer = "Airbus Helicopters",
            category = AircraftCategory.HELICOPTER,
            description = "A heavy-duty medium twin-engine helicopter relied upon for offshore oil support, VIP transport, and search and rescue.",
            specs = "Crew: 2 | Range: 841km | Cruise: 262km/h | Engines: 2x Makila 1A1 turboshafts",
            photoAsset = "aircraft/S92.jpg", icaoTypeCodes = listOf("AS32")
        ),
        AircraftReference(
            id = "b412", name = "Bell 412", manufacturer = "Bell Helicopter",
            category = AircraftCategory.HELICOPTER,
            description = "A rugged twin-engine utility helicopter descended from the Huey with a four-blade main rotor for improved hot-and-high performance.",
            specs = "Crew: 1-2 | Range: 650km | Cruise: 226km/h | Engines: 2x PT6T-3D Twin-Pac",
            photoAsset = "aircraft/UH1.jpg", icaoTypeCodes = listOf("B412")
        ),
        // Bomber variants
        AircraftReference(
            id = "b1b", name = "Rockwell B-1B Lancer", manufacturer = "Rockwell International",
            category = AircraftCategory.FIGHTER,
            description = "A supersonic variable-sweep wing heavy bomber carrying the largest payload of guided and unguided weapons in the US Air Force.",
            specs = "Crew: 4 | Range: 9,400km | Cruise: Mach 1.2 | Engines: 4x GE F101-GE-102 turbofans",
            photoAsset = "aircraft/B1.jpg", icaoTypeCodes = listOf("B1B")
        ),
        AircraftReference(
            id = "b52h", name = "Boeing B-52H Stratofortress", manufacturer = "Boeing",
            category = AircraftCategory.FIGHTER,
            description = "A massive eight-engine strategic bomber that has served as the backbone of the US strategic deterrent for over 60 years.",
            specs = "Crew: 5 | Range: 14,200km | Cruise: 844km/h | Engines: 8x P&W TF33-P-3/103 turbofans",
            photoAsset = "aircraft/B52.jpg", icaoTypeCodes = listOf("B52H")
        )
    )

    /** Get aircraft filtered by category */
    fun byCategory(category: AircraftCategory): List<AircraftReference> =
        allAircraft.filter { it.category == category }

    /** Search aircraft by name, manufacturer, or description (case-insensitive) */
    fun search(query: String): List<AircraftReference> {
        val q = query.lowercase()
        return allAircraft.filter {
            it.name.lowercase().contains(q) ||
                it.manufacturer.lowercase().contains(q) ||
                it.description.lowercase().contains(q) ||
                it.icaoTypeCodes.any { code -> code.lowercase().contains(q) }
        }
    }

    /** Find matching aircraft reference by ICAO type code */
    fun matchByTypeCode(typeCode: String): AircraftReference? {
        val normalized = typeCode.uppercase()
        return allAircraft.firstOrNull { ref ->
            ref.icaoTypeCodes.any { it.uppercase() == normalized }
        }
    }

    /** Get all distinct categories that have aircraft */
    val availableCategories: List<AircraftCategory>
        get() = allAircraft.map { it.category }.distinct()
}
