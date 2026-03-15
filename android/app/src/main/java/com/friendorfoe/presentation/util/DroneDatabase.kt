package com.friendorfoe.presentation.util

/**
 * Built-in drone reference database for visual identification.
 *
 * Contains specs, descriptions, and photo asset references for major consumer,
 * enterprise, and military drone types. Used by the Drone Reference Screen to
 * help users identify unknown drones, and linked from drone detail cards when
 * the manufacturer or model is unknown.
 */

data class DroneReference(
    val id: String,
    val name: String,
    val manufacturer: String,
    val category: DroneCategory,
    val description: String,
    val specs: String,
    val photoAsset: String,
    val wifiPatterns: List<String>
)

enum class DroneCategory(val label: String) {
    CONSUMER("Consumer"),
    ENTERPRISE("Enterprise"),
    RACING_FPV("Racing / FPV"),
    MILITARY_RECON("Military Recon"),
    MILITARY_STRIKE("Military Strike"),
    LOITERING_MUNITION("Loitering Munition")
}

object DroneDatabase {

    val allDrones: List<DroneReference> = listOf(
        // ── Consumer Drones ──
        DroneReference(
            id = "dji_mavic3",
            name = "DJI Mavic 3",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Foldable prosumer quadcopter with Hasselblad camera and 46-minute flight time. Dual camera system with telephoto zoom.",
            specs = "Weight: 895g | Range: 15km | Max speed: 75km/h | Flight time: 46min",
            photoAsset = "drones/dji_mavic3.jpg",
            wifiPatterns = listOf("DJI-", "MAVIC-")
        ),
        DroneReference(
            id = "dji_mini4",
            name = "DJI Mini 4 Pro",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Sub-250g consumer drone requiring no FAA registration for recreational use. Omnidirectional obstacle sensing despite tiny size.",
            specs = "Weight: 249g | Range: 20km | Max speed: 57km/h | Flight time: 34min",
            photoAsset = "drones/dji_mini4.jpg",
            wifiPatterns = listOf("DJI-", "MINI-")
        ),
        DroneReference(
            id = "dji_phantom4",
            name = "DJI Phantom 4 Pro",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Classic white quadcopter that defined consumer drones. 1-inch sensor camera with mechanical shutter. Now discontinued but still widely flown.",
            specs = "Weight: 1388g | Range: 7km | Max speed: 72km/h | Flight time: 30min",
            photoAsset = "drones/dji_phantom4.jpg",
            wifiPatterns = listOf("DJI-", "PHANTOM-")
        ),
        DroneReference(
            id = "dji_inspire3",
            name = "DJI Inspire 3",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Large cinema drone with full-frame Zenmuse X9-8K camera. Transforming design with retractable landing gear for unobstructed 360-degree camera view.",
            specs = "Weight: 3995g | Range: 15km | Max speed: 94km/h | Flight time: 28min",
            photoAsset = "drones/dji_inspire3.jpg",
            wifiPatterns = listOf("DJI-", "INSPIRE-")
        ),
        DroneReference(
            id = "dji_fpv",
            name = "DJI FPV / Avata",
            manufacturer = "DJI",
            category = DroneCategory.RACING_FPV,
            description = "First-person-view racing drone with immersive goggles. Emergency brake and hover. The Avata is the smaller, ducted-fan indoor-friendly variant.",
            specs = "Weight: 795g | Range: 10km | Max speed: 140km/h | Flight time: 20min",
            photoAsset = "drones/dji_fpv.jpg",
            wifiPatterns = listOf("DJI-", "FPV-", "AVATA-")
        ),
        DroneReference(
            id = "dji_matrice",
            name = "DJI Matrice 300/350 RTK",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Heavy-duty enterprise drone for mapping, inspection, and search-and-rescue. IP45 weather resistance. Supports multiple payload configurations.",
            specs = "Weight: 6300g | Range: 15km | Max speed: 82km/h | Flight time: 55min",
            photoAsset = "drones/dji_matrice.jpg",
            wifiPatterns = listOf("DJI-")
        ),
        DroneReference(
            id = "dji_air",
            name = "DJI Air 3",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Mid-range foldable drone with dual cameras — wide and medium telephoto. Good balance of portability, image quality, and flight time.",
            specs = "Weight: 720g | Range: 20km | Max speed: 75km/h | Flight time: 46min",
            photoAsset = "drones/dji_air.jpg",
            wifiPatterns = listOf("DJI-")
        ),
        DroneReference(
            id = "skydio2",
            name = "Skydio 2+",
            manufacturer = "Skydio",
            category = DroneCategory.CONSUMER,
            description = "US-made autonomous drone with industry-leading obstacle avoidance using six navigation cameras. Popular for autonomous tracking shots.",
            specs = "Weight: 800g | Range: 6km | Max speed: 58km/h | Flight time: 27min",
            photoAsset = "drones/skydio2.jpg",
            wifiPatterns = listOf("SKYDIO-")
        ),
        DroneReference(
            id = "skydio_x10",
            name = "Skydio X10",
            manufacturer = "Skydio",
            category = DroneCategory.ENTERPRISE,
            description = "Enterprise-grade US drone with 65x zoom, thermal imaging, and edge computing. Designed for defense, public safety, and critical infrastructure.",
            specs = "Weight: 2200g | Range: 12km | Max speed: 65km/h | Flight time: 35min",
            photoAsset = "drones/skydio_x10.jpg",
            wifiPatterns = listOf("SKYDIO-")
        ),
        DroneReference(
            id = "parrot_anafi",
            name = "Parrot Anafi",
            manufacturer = "Parrot",
            category = DroneCategory.CONSUMER,
            description = "French-made foldable drone with 180-degree tilt gimbal that can look straight up. Compact and lightweight.",
            specs = "Weight: 320g | Range: 4km | Max speed: 55km/h | Flight time: 25min",
            photoAsset = "drones/parrot_anafi.jpg",
            wifiPatterns = listOf("PARROT-", "ANAFI-")
        ),
        DroneReference(
            id = "autel_evo2",
            name = "Autel EVO II Pro",
            manufacturer = "Autel",
            category = DroneCategory.CONSUMER,
            description = "Orange enterprise drone with 6K camera and 1-inch sensor. Strong DJI competitor with longer flight time and US-friendly support.",
            specs = "Weight: 1191g | Range: 9km | Max speed: 72km/h | Flight time: 42min",
            photoAsset = "drones/autel_evo2.jpg",
            wifiPatterns = listOf("AUTEL-", "EVO-")
        ),
        DroneReference(
            id = "autel_evo_nano",
            name = "Autel EVO Nano+",
            manufacturer = "Autel",
            category = DroneCategory.CONSUMER,
            description = "Sub-250g alternative to DJI Mini series with RYYB sensor for better low-light performance. Three-way obstacle avoidance.",
            specs = "Weight: 249g | Range: 10km | Max speed: 54km/h | Flight time: 28min",
            photoAsset = "drones/autel_evo_nano.jpg",
            wifiPatterns = listOf("AUTEL-", "EVO-")
        ),
        DroneReference(
            id = "holy_stone",
            name = "Holy Stone HS720",
            manufacturer = "Holy Stone",
            category = DroneCategory.CONSUMER,
            description = "Budget-friendly GPS drone popular on Amazon. Foldable with 4K camera. Good entry-level option for beginners.",
            specs = "Weight: 460g | Range: 1km | Max speed: 40km/h | Flight time: 26min",
            photoAsset = "drones/holy_stone.jpg",
            wifiPatterns = listOf("HOLY", "HS-")
        ),
        DroneReference(
            id = "hubsan",
            name = "Hubsan Zino Mini Pro",
            manufacturer = "Hubsan",
            category = DroneCategory.CONSUMER,
            description = "Budget GPS drone with obstacle avoidance. Sub-250g design at a fraction of DJI pricing.",
            specs = "Weight: 249g | Range: 10km | Max speed: 57km/h | Flight time: 40min",
            photoAsset = "drones/hubsan.jpg",
            wifiPatterns = listOf("HUBSAN-")
        ),
        DroneReference(
            id = "yuneec_typhoon",
            name = "Yuneec Typhoon H Plus",
            manufacturer = "Yuneec",
            category = DroneCategory.CONSUMER,
            description = "Hexacopter with retractable landing gear and 360-degree rotating camera. Six rotors provide redundancy — can land safely with one motor out.",
            specs = "Weight: 1985g | Range: 1.6km | Max speed: 75km/h | Flight time: 28min",
            photoAsset = "drones/yuneec_typhoon.jpg",
            wifiPatterns = listOf("YUNEEC-", "TYPHOON-")
        ),
        DroneReference(
            id = "fimi_x8",
            name = "FIMI X8 Mini",
            manufacturer = "FIMI/Xiaomi",
            category = DroneCategory.CONSUMER,
            description = "Part of the Xiaomi ecosystem. Compact GPS drone with 4K video and 3-axis gimbal at an aggressive price point.",
            specs = "Weight: 258g | Range: 8km | Max speed: 57km/h | Flight time: 31min",
            photoAsset = "drones/fimi_x8.jpg",
            wifiPatterns = listOf("FIMI-", "XIAOMI-")
        ),
        DroneReference(
            id = "hoverair_x1",
            name = "HOVERAir X1",
            manufacturer = "Zero Zero Robotics",
            category = DroneCategory.CONSUMER,
            description = "Palm-sized selfie drone that takes off from your hand. No controller needed — fully autonomous flight paths for social media content.",
            specs = "Weight: 125g | Range: 30m | Max speed: 25km/h | Flight time: 11min",
            photoAsset = "drones/hoverair_x1.jpg",
            wifiPatterns = listOf("HOVERAIR", "HOVER-", "HOVER AIR", "HOVER_AIR")
        ),
        DroneReference(
            id = "tello",
            name = "Ryze/DJI Tello",
            manufacturer = "Ryze/DJI",
            category = DroneCategory.CONSUMER,
            description = "Mini educational drone powered by DJI flight tech. Programmable via Scratch and Python. Popular for STEM education.",
            specs = "Weight: 80g | Range: 100m | Max speed: 28km/h | Flight time: 13min",
            photoAsset = "drones/tello.jpg",
            wifiPatterns = listOf("TELLO-")
        ),

        // ── Military / Threat Drones ──
        DroneReference(
            id = "shahed_136",
            name = "Shahed-136 / Geran-2",
            manufacturer = "HESA (Iran)",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Iranian-made delta-wing kamikaze drone used extensively in the Russia-Ukraine war. Cheap, mass-produced one-way attack drone with GPS guidance. Flies in swarms to overwhelm air defenses.",
            specs = "Weight: 200kg | Range: 2500km | Max speed: 185km/h | Warhead: 40-50kg",
            photoAsset = "drones/shahed_136.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "shahed_129",
            name = "Shahed-129",
            manufacturer = "HESA (Iran)",
            category = DroneCategory.MILITARY_RECON,
            description = "Iranian medium-altitude long-endurance (MALE) drone for surveillance and strike. Resembles the MQ-1 Predator. Can carry Sadid precision-guided munitions.",
            specs = "Weight: 450kg | Range: 1700km | Max speed: 200km/h | Endurance: 24h",
            photoAsset = "drones/shahed_129.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "mohajer_6",
            name = "Mohajer-6",
            manufacturer = "Qods Aviation (Iran)",
            category = DroneCategory.MILITARY_RECON,
            description = "Iranian tactical reconnaissance and strike drone. Twin-boom pusher configuration. Exported to several countries and used in multiple conflicts.",
            specs = "Weight: 670kg | Range: 200km | Max speed: 200km/h | Endurance: 12h",
            photoAsset = "drones/mohajer_6.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "bayraktar_tb2",
            name = "Bayraktar TB2",
            manufacturer = "Baykar (Turkey)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Turkish tactical UCAV that proved devastating in Libya, Nagorno-Karabakh, and Ukraine. Carries laser-guided MAM munitions. Changed modern warfare doctrine.",
            specs = "Weight: 650kg | Range: 150km | Max speed: 220km/h | Endurance: 27h",
            photoAsset = "drones/bayraktar_tb2.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "bayraktar_akinci",
            name = "Bayraktar Akinci",
            manufacturer = "Baykar (Turkey)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Turkish heavy UCAV with twin engines and AI-assisted targeting. Can carry cruise missiles and SOM-J standoff munitions. Next generation after TB2.",
            specs = "Weight: 5500kg | Range: 300km | Max speed: 361km/h | Endurance: 24h | Ceiling: 40,000ft",
            photoAsset = "drones/bayraktar_akinci.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "orion_uav",
            name = "Orion (Pacer)",
            manufacturer = "Kronshtadt (Russia)",
            category = DroneCategory.MILITARY_RECON,
            description = "Russian medium-altitude reconnaissance and strike drone. Used in Ukraine for surveillance and precision strikes with guided munitions.",
            specs = "Weight: 1000kg | Range: 250km | Max speed: 200km/h | Endurance: 24h",
            photoAsset = "drones/orion_uav.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "lancet",
            name = "ZALA Lancet",
            manufacturer = "ZALA Aero (Russia)",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Russian loitering munition used extensively in Ukraine. Small, cheap kamikaze drone with TV/IR seeker. Effective against artillery, vehicles, and trenches.",
            specs = "Weight: 12kg | Range: 40km | Max speed: 110km/h | Warhead: 3-5kg",
            photoAsset = "drones/lancet.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "wing_loong",
            name = "Wing Loong II",
            manufacturer = "CASC/Chengdu (China)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Chinese MALE UCAV exported widely as an affordable MQ-9 alternative. Used by UAE, Saudi Arabia, Egypt, Pakistan, and others.",
            specs = "Weight: 4200kg | Range: 4000km | Max speed: 370km/h | Endurance: 32h",
            photoAsset = "drones/wing_loong.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "mq9_reaper",
            name = "MQ-9 Reaper",
            manufacturer = "General Atomics (USA)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Primary US military hunter-killer drone. Carries Hellfire missiles and precision bombs. Used extensively in counter-terrorism operations worldwide.",
            specs = "Weight: 4760kg | Range: 1850km | Max speed: 482km/h | Endurance: 27h | Ceiling: 50,000ft",
            photoAsset = "drones/mq9_reaper.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "mq1_predator",
            name = "MQ-1 Predator",
            manufacturer = "General Atomics (USA)",
            category = DroneCategory.MILITARY_RECON,
            description = "The drone that started it all. Originally designed for reconnaissance, later armed with Hellfire missiles. Retired from USAF in 2018 but still in use by other countries.",
            specs = "Weight: 1020kg | Range: 1240km | Max speed: 217km/h | Endurance: 24h",
            photoAsset = "drones/mq1_predator.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "rq4_global_hawk",
            name = "RQ-4 Global Hawk",
            manufacturer = "Northrop Grumman (USA)",
            category = DroneCategory.MILITARY_RECON,
            description = "High-altitude, long-endurance ISR platform. Flies at 60,000ft for 30+ hours. Provides wide-area surveillance with synthetic aperture radar and electro-optical sensors.",
            specs = "Weight: 14628kg | Range: 22,780km | Max speed: 629km/h | Endurance: 34h | Ceiling: 60,000ft",
            photoAsset = "drones/rq4_global_hawk.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "switchblade",
            name = "AeroVironment Switchblade 600",
            manufacturer = "AeroVironment (USA)",
            category = DroneCategory.LOITERING_MUNITION,
            description = "US-made tube-launched loitering munition. Backpack-portable. Can loiter for 40+ minutes before striking armored vehicles. Sent to Ukraine in large numbers.",
            specs = "Weight: 23kg | Range: 40km | Max speed: 185km/h | Loiter: 40min | Warhead: anti-armor",
            photoAsset = "drones/switchblade.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "heron",
            name = "IAI Heron",
            manufacturer = "Israel Aerospace Industries",
            category = DroneCategory.MILITARY_RECON,
            description = "Israeli MALE drone widely exported and used by India, Germany, Australia, Turkey, and others. Long endurance with multi-sensor payload.",
            specs = "Weight: 1150kg | Range: 1000km | Max speed: 207km/h | Endurance: 45h",
            photoAsset = "drones/heron.jpg",
            wifiPatterns = emptyList()
        )
    )

    /** Get drones filtered by category */
    fun byCategory(category: DroneCategory): List<DroneReference> =
        allDrones.filter { it.category == category }

    /** Search drones by name or manufacturer (case-insensitive) */
    fun search(query: String): List<DroneReference> {
        val q = query.lowercase()
        return allDrones.filter {
            it.name.lowercase().contains(q) ||
                it.manufacturer.lowercase().contains(q) ||
                it.description.lowercase().contains(q)
        }
    }

    /** Find matching drone references by WiFi SSID */
    fun matchByWifiSsid(ssid: String): List<DroneReference> {
        val upper = ssid.uppercase()
        return allDrones.filter { drone ->
            drone.wifiPatterns.any { pattern ->
                upper.startsWith(pattern.uppercase())
            }
        }
    }

    /** Find matching drone references by manufacturer name */
    fun matchByManufacturer(manufacturer: String): List<DroneReference> {
        val mfr = manufacturer.lowercase()
        return allDrones.filter { it.manufacturer.lowercase().contains(mfr) }
    }

    /** Get all distinct categories that have drones */
    val availableCategories: List<DroneCategory>
        get() = allDrones.map { it.category }.distinct()
}
