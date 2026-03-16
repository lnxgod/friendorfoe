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
    LOITERING_MUNITION("Loitering Munition"),
    FPV_COMBAT("FPV Combat")
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
            wifiPatterns = listOf("DJI-", "MATRICE-")
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
            id = "hoverair_x1_pro",
            name = "HOVERAir X1 Pro",
            manufacturer = "Zero Zero Robotics",
            category = DroneCategory.CONSUMER,
            description = "Enhanced palm-sized selfie drone with 4K HDR camera and WiFi 6. " +
                "Longer range and flight time than X1. May support FAA Remote ID via firmware update.",
            specs = "Weight: 135g | Range: 100m | Max speed: 36km/h | Flight time: 18min | WiFi 6 | 4K HDR",
            photoAsset = "drones/hoverair_x1.jpg",
            wifiPatterns = listOf("HOVERAIR", "HOVER-", "X1PRO", "X1-PRO", "X1 PRO")
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

        // ── Budget & FPV Drones ──
        DroneReference(
            id = "snaptain_sp7100",
            name = "Snaptain SP7100",
            manufacturer = "Snaptain",
            category = DroneCategory.CONSUMER,
            description = "Budget foldable GPS drone with 4K camera and brushless motors. Popular Amazon best-seller for beginners with follow-me and return-to-home.",
            specs = "Weight: 450g | Range: 1km | Max speed: 36km/h | Flight time: 26min",
            photoAsset = "drones/snaptain_sp7100.jpg",
            wifiPatterns = listOf("SNAPTAIN-")
        ),
        DroneReference(
            id = "potensic_dreamer_pro",
            name = "Potensic Dreamer Pro",
            manufacturer = "Potensic",
            category = DroneCategory.CONSUMER,
            description = "Mid-range GPS drone with 3-axis gimbal and Sony sensor. Offers features typically found in higher-priced drones at a budget-friendly price point.",
            specs = "Weight: 770g | Range: 2km | Max speed: 43km/h | Flight time: 28min",
            photoAsset = "drones/potensic_dreamer_pro.jpg",
            wifiPatterns = listOf("POTENSIC-")
        ),
        DroneReference(
            id = "ruko_f11_gim2",
            name = "Ruko F11 GIM2",
            manufacturer = "Ruko",
            category = DroneCategory.CONSUMER,
            description = "Budget GPS drone with 2-axis gimbal and 4K camera. Long flight time and stable hovering make it popular for casual aerial photography.",
            specs = "Weight: 540g | Range: 3km | Max speed: 40km/h | Flight time: 28min",
            photoAsset = "drones/ruko_f11_gim2.jpg",
            wifiPatterns = listOf("RUKO-")
        ),
        DroneReference(
            id = "syma_x500",
            name = "Syma X500",
            manufacturer = "Syma",
            category = DroneCategory.CONSUMER,
            description = "Beginner-friendly GPS drone with 4K camera and optical flow positioning. One of the most affordable GPS drones available. Good for learning to fly.",
            specs = "Weight: 370g | Range: 500m | Max speed: 32km/h | Flight time: 20min",
            photoAsset = "drones/syma_x500.jpg",
            wifiPatterns = listOf("SYMA-")
        ),
        DroneReference(
            id = "eachine_e520s",
            name = "Eachine E520S",
            manufacturer = "Eachine",
            category = DroneCategory.CONSUMER,
            description = "Ultra-budget foldable drone resembling a mini Mavic. WiFi FPV with 4K camera. Extremely popular entry-level drone sold under many brand names including E58, E88, and E99 variants.",
            specs = "Weight: 270g | Range: 300m | Max speed: 30km/h | Flight time: 16min",
            photoAsset = "drones/eachine_e520s.jpg",
            wifiPatterns = listOf("EACHINE-", "E58-", "E88-", "E99-")
        ),
        DroneReference(
            id = "jjrc_x20",
            name = "JJRC X20",
            manufacturer = "JJRC",
            category = DroneCategory.CONSUMER,
            description = "Budget GPS drone with 3-axis gimbal and EIS stabilization. JJRC is one of the largest Chinese drone brands, offering DJI-like features at a fraction of the price.",
            specs = "Weight: 560g | Range: 1.2km | Max speed: 40km/h | Flight time: 25min",
            photoAsset = "drones/jjrc_x20.jpg",
            wifiPatterns = listOf("JJRC-")
        ),
        DroneReference(
            id = "mjx_bugs_16_pro",
            name = "MJX Bugs 16 Pro",
            manufacturer = "MJX",
            category = DroneCategory.CONSUMER,
            description = "Budget GPS drone with 3-axis EIS and 4K camera. The Bugs series is well-known in the budget drone community for offering solid GPS performance at low cost.",
            specs = "Weight: 520g | Range: 1.6km | Max speed: 43km/h | Flight time: 28min",
            photoAsset = "drones/mjx_bugs_16_pro.jpg",
            wifiPatterns = listOf("MJX-")
        ),
        DroneReference(
            id = "visuo_xs816",
            name = "Visuo XS816",
            manufacturer = "Visuo",
            category = DroneCategory.CONSUMER,
            description = "Ultra-budget foldable drone with dual cameras and optical flow. Known as one of the cheapest drones with gesture control and V-sign selfie mode.",
            specs = "Weight: 200g | Range: 100m | Max speed: 25km/h | Flight time: 15min",
            photoAsset = "drones/visuo_xs816.jpg",
            wifiPatterns = listOf("VISUO-")
        ),
        DroneReference(
            id = "sjrc_f22s_4k_pro",
            name = "SJRC F22S 4K Pro",
            manufacturer = "SJRC",
            category = DroneCategory.CONSUMER,
            description = "Budget GPS drone with laser obstacle avoidance and 2-axis gimbal. SJRC offers competitive features at budget pricing including 3.5km range.",
            specs = "Weight: 520g | Range: 3.5km | Max speed: 43km/h | Flight time: 35min",
            photoAsset = "drones/sjrc_f22s_4k_pro.jpg",
            wifiPatterns = listOf("SJRC-")
        ),
        DroneReference(
            id = "4drc_f13",
            name = "4DRC F13",
            manufacturer = "4DRC",
            category = DroneCategory.CONSUMER,
            description = "Budget foldable drone with brushless motors and GPS. 4DRC is a fast-growing budget brand on Amazon known for aggressive pricing and frequent new models.",
            specs = "Weight: 400g | Range: 1km | Max speed: 36km/h | Flight time: 24min",
            photoAsset = "drones/4drc_f13.jpg",
            wifiPatterns = listOf("4DRC-")
        ),
        DroneReference(
            id = "wingsland_s6",
            name = "Wingsland S6",
            manufacturer = "Wingsland",
            category = DroneCategory.CONSUMER,
            description = "Pocket-sized foldable drone with modular accessories. Supports swappable attachments including searchlight, emoji display, and water gun modules.",
            specs = "Weight: 230g | Range: 100m | Max speed: 32km/h | Flight time: 10min",
            photoAsset = "drones/wingsland_s6.jpg",
            wifiPatterns = listOf("WINGSLAND-")
        ),
        DroneReference(
            id = "betafpv_cetus_pro",
            name = "BetaFPV Cetus Pro",
            manufacturer = "BetaFPV",
            category = DroneCategory.RACING_FPV,
            description = "Beginner FPV whoop drone with altitude hold and turtle mode. Part of the popular Cetus FPV training kit with goggles and radio. Ducted propellers for safe indoor flying.",
            specs = "Weight: 33g | Range: 200m | Max speed: 40km/h | Flight time: 5min",
            photoAsset = "drones/betafpv_cetus_pro.jpg",
            wifiPatterns = listOf("BETAFPV-")
        ),
        DroneReference(
            id = "geprc_cinelog35",
            name = "GEPRC CineLog35",
            manufacturer = "GEPRC",
            category = DroneCategory.RACING_FPV,
            description = "Popular 3.5-inch cinewhoop for smooth cinematic FPV footage. Ducted propellers reduce noise and improve safety. Carries GoPro or similar action cameras.",
            specs = "Weight: 200g (no battery) | Max speed: 100km/h | Flight time: 8min",
            photoAsset = "drones/geprc_cinelog35.jpg",
            wifiPatterns = listOf("GEPRC-")
        ),
        DroneReference(
            id = "emax_tinyhawk3",
            name = "EMAX Tinyhawk III",
            manufacturer = "EMAX",
            category = DroneCategory.RACING_FPV,
            description = "Micro FPV racing quad for indoor and outdoor flying. Third generation of the iconic Tinyhawk series. Great trainer for learning FPV flying skills.",
            specs = "Weight: 37g | Range: 200m | Max speed: 50km/h | Flight time: 5min",
            photoAsset = "drones/emax_tinyhawk3.jpg",
            wifiPatterns = listOf("EMAX-")
        ),
        DroneReference(
            id = "iflight_nazgul5_v3",
            name = "iFlight Nazgul5 V3",
            manufacturer = "iFlight",
            category = DroneCategory.RACING_FPV,
            description = "5-inch freestyle FPV quad known for durability and performance. The Nazgul series is a community favorite for aggressive freestyle flying and racing.",
            specs = "Weight: 380g (no battery) | Max speed: 160km/h | Flight time: 6min",
            photoAsset = "drones/iflight_nazgul5_v3.jpg",
            wifiPatterns = listOf("IFLIGHT-")
        ),
        DroneReference(
            id = "walkera_vitus_320",
            name = "Walkera Vitus 320",
            manufacturer = "Walkera",
            category = DroneCategory.CONSUMER,
            description = "Compact foldable drone with obstacle avoidance and 4K camera. Walkera was a pioneering Chinese drone brand known for the QR X350 and later Vitus series.",
            specs = "Weight: 480g | Range: 2km | Max speed: 54km/h | Flight time: 20min",
            photoAsset = "drones/walkera_vitus_320.jpg",
            wifiPatterns = listOf("WALKERA-")
        ),
        DroneReference(
            id = "blade_inductrix_fpv",
            name = "Blade Inductrix FPV+",
            manufacturer = "Blade",
            category = DroneCategory.RACING_FPV,
            description = "Tiny ducted whoop drone from Blade/Horizon Hobby. One of the first popular micro FPV quads. Safe indoor flyer with prop guards for beginners.",
            specs = "Weight: 30g | Range: 50m | Max speed: 25km/h | Flight time: 4min",
            photoAsset = "drones/blade_inductrix_fpv.jpg",
            wifiPatterns = listOf("BLADE-")
        ),
        DroneReference(
            id = "flywoo_explorer_lr",
            name = "Flywoo Explorer LR",
            manufacturer = "Flywoo",
            category = DroneCategory.RACING_FPV,
            description = "Long-range FPV quad designed for cinematic exploration. Efficient design with long flight times for an FPV drone. Popular for mountain and landscape cruising.",
            specs = "Weight: 210g (no battery) | Max speed: 100km/h | Flight time: 15min",
            photoAsset = "drones/flywoo_explorer_lr.jpg",
            wifiPatterns = listOf("FLYWOO-")
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
        ),

        // ── New Consumer Drones ──
        DroneReference(
            id = "dji_neo",
            name = "DJI Neo",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Ultra-compact palm-sized drone under 135g. Controller-free AI flight modes for selfies and vlogging. Takes off from your hand and returns automatically.",
            specs = "Weight: 135g | Range: 6km | Max speed: 33km/h | Flight time: 18min",
            photoAsset = "drones/dji_neo.jpg",
            wifiPatterns = listOf("DJI-", "NEO-")
        ),
        DroneReference(
            id = "dji_mini3",
            name = "DJI Mini 3",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Budget sub-250g drone with true vertical shooting for social media. No obstacle avoidance but lightweight and affordable. Great for beginners.",
            specs = "Weight: 248g | Range: 10km | Max speed: 57km/h | Flight time: 38min",
            photoAsset = "drones/dji_mini3.jpg",
            wifiPatterns = listOf("DJI-", "MINI-")
        ),
        DroneReference(
            id = "dji_mavic_air2s",
            name = "DJI Mavic Air 2S",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Compact foldable drone with 1-inch sensor and 5.4K video. MasterShots automated cinematography. Four-directional obstacle sensing. Popular prosumer choice.",
            specs = "Weight: 595g | Range: 18.5km | Max speed: 68km/h | Flight time: 31min",
            photoAsset = "drones/dji_mavic_air2s.jpg",
            wifiPatterns = listOf("DJI-")
        ),
        DroneReference(
            id = "dji_air2",
            name = "DJI Air 2",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Predecessor to Air 2S. 48MP photos, 4K/60fps video, and FocusTrack subject tracking. Solid mid-range option that brought pro features to a compact form factor.",
            specs = "Weight: 570g | Range: 18.5km | Max speed: 68km/h | Flight time: 34min",
            photoAsset = "drones/dji_air2.jpg",
            wifiPatterns = listOf("DJI-")
        ),
        DroneReference(
            id = "parrot_bebop2",
            name = "Parrot Bebop 2",
            manufacturer = "Parrot",
            category = DroneCategory.CONSUMER,
            description = "French-made GPS drone with fish-eye camera and digital stabilization. Lightweight at 500g with FPV goggle support. Now discontinued but still flown by enthusiasts.",
            specs = "Weight: 500g | Range: 2km | Max speed: 60km/h | Flight time: 25min",
            photoAsset = "drones/parrot_bebop2.jpg",
            wifiPatterns = listOf("PARROT-", "BEBOP-")
        ),
        DroneReference(
            id = "skydio_x2",
            name = "Skydio X2",
            manufacturer = "Skydio",
            category = DroneCategory.ENTERPRISE,
            description = "US-made enterprise drone with dual thermal/visual cameras. Foldable, rucksack-portable. Used by US Army Short Range Reconnaissance program. AI-powered autonomy.",
            specs = "Weight: 1500g | Range: 6km | Max speed: 58km/h | Flight time: 35min",
            photoAsset = "drones/skydio_x2.jpg",
            wifiPatterns = listOf("SKYDIO-")
        ),

        // ── New Enterprise Drones ──
        DroneReference(
            id = "autel_evo_max4t",
            name = "Autel EVO Max 4T",
            manufacturer = "Autel",
            category = DroneCategory.ENTERPRISE,
            description = "Multi-sensor enterprise drone with wide, zoom, thermal, and laser rangefinder cameras. Omnidirectional obstacle avoidance. Built for public safety and inspection.",
            specs = "Weight: 1164g | Range: 20km | Max speed: 75km/h | Flight time: 42min",
            photoAsset = "drones/autel_evo_max4t.jpg",
            wifiPatterns = listOf("AUTEL-", "EVO-")
        ),
        DroneReference(
            id = "autel_dragonfish",
            name = "Autel Dragonfish",
            manufacturer = "Autel",
            category = DroneCategory.ENTERPRISE,
            description = "Fixed-wing VTOL drone for long-range mapping and surveillance. Transitions between hover and forward flight. Up to 126 minutes flight time with 18km range.",
            specs = "Weight: 9kg | Range: 18km | Max speed: 108km/h | Flight time: 126min",
            photoAsset = "drones/autel_dragonfish.jpg",
            wifiPatterns = listOf("AUTEL-")
        ),
        DroneReference(
            id = "dji_agras_t40",
            name = "DJI Agras T40",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Large agricultural spray drone with 40L tank and 20m spray width. Coaxial twin-rotor design. Spreads granular fertilizer and pesticide with precision. Leading ag-drone worldwide.",
            specs = "Weight: 52kg (loaded) | Spray rate: 16L/min | Max speed: 41km/h | Flight time: ~10min (loaded)",
            photoAsset = "drones/dji_agras_t40.jpg",
            wifiPatterns = listOf("DJI-", "AGRAS-")
        ),
        DroneReference(
            id = "wingtraone",
            name = "WingtraOne",
            manufacturer = "Wingtra",
            category = DroneCategory.ENTERPRISE,
            description = "Swiss-made VTOL mapping drone with PPK/RTK survey-grade accuracy. Tailsitter design — takes off vertically, transitions to fixed-wing for efficient long-range mapping.",
            specs = "Weight: 4.5kg | Range: 70km | Max speed: 120km/h | Flight time: 59min",
            photoAsset = "drones/wingtraone.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "zipline_p2",
            name = "Zipline P2 Zip",
            manufacturer = "Zipline",
            category = DroneCategory.ENTERPRISE,
            description = "Autonomous delivery drone used for medical supply drops in Rwanda, Ghana, and US. Launches from catapult, drops packages by parachute. Over 1 million deliveries made.",
            specs = "Weight: 20kg | Range: 160km round-trip | Max speed: 128km/h | Payload: 1.8kg",
            photoAsset = "drones/zipline_p2.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "wing_delivery",
            name = "Wing (Alphabet) Delivery Drone",
            manufacturer = "Wing (Alphabet/Google)",
            category = DroneCategory.ENTERPRISE,
            description = "Autonomous delivery drone from Google's parent company. Lowers packages on a tether from hover. Operating commercially in Australia, Finland, and parts of the US.",
            specs = "Weight: 5.2kg | Range: 12km | Max speed: 113km/h | Payload: 1.2kg",
            photoAsset = "drones/wing_delivery.jpg",
            wifiPatterns = listOf("WING-")
        ),

        // ── New Military Drones ──
        DroneReference(
            id = "mq25_stingray",
            name = "MQ-25 Stingray",
            manufacturer = "Boeing (USA)",
            category = DroneCategory.MILITARY_RECON,
            description = "US Navy's first carrier-based unmanned tanker. Refuels F/A-18s and F-35Cs in flight, extending carrier air wing range by up to 700nm. Stealthy flying-wing design.",
            specs = "Weight: 20,200kg | Range: 900km | Max speed: 890km/h | Fuel offload: 6,800kg",
            photoAsset = "drones/mq25_stingray.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "mq1c_gray_eagle",
            name = "MQ-1C Gray Eagle",
            manufacturer = "General Atomics (USA)",
            category = DroneCategory.MILITARY_RECON,
            description = "US Army's primary ISR and strike UAS. Extended-range Predator derivative with heavier payload and longer endurance. Carries Hellfire missiles and GBU-44 Viper Strike.",
            specs = "Weight: 1633kg | Range: 400km | Max speed: 280km/h | Endurance: 25h",
            photoAsset = "drones/mq1c_gray_eagle.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "mq4c_triton",
            name = "MQ-4C Triton",
            manufacturer = "Northrop Grumman (USA)",
            category = DroneCategory.MILITARY_RECON,
            description = "Naval variant of Global Hawk for maritime ISR. Flies at 56,000ft for 24+ hours, covering 7 million square km per mission. Provides persistent maritime domain awareness.",
            specs = "Weight: 14,628kg | Range: 15,186km | Max speed: 611km/h | Endurance: 24h | Ceiling: 56,000ft",
            photoAsset = "drones/mq4c_triton.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "xq58_valkyrie",
            name = "XQ-58 Valkyrie",
            manufacturer = "Kratos (USA)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Low-cost attritable autonomous combat drone. Designed as 'loyal wingman' to accompany manned fighters. Subsonic, stealthy, and expendable. Can carry JDAMs and small-diameter bombs.",
            specs = "Weight: 2,722kg | Range: 3,941km | Max speed: Mach 0.85 | Ceiling: 45,000ft",
            photoAsset = "drones/xq58_valkyrie.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "bayraktar_tb3",
            name = "Bayraktar TB3",
            manufacturer = "Baykar (Turkey)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Carrier-capable evolution of TB2 with folding wings for naval operations. Designed for Turkey's TCG Anadolu amphibious assault ship. STOL capability for short flight decks.",
            specs = "Weight: 1450kg | Range: 185km | Max speed: 260km/h | Endurance: 24h",
            photoAsset = "drones/bayraktar_tb3.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "anka_s",
            name = "TAI Anka-S",
            manufacturer = "Turkish Aerospace (Turkey)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Turkish MALE UCAV with satellite communication link for beyond-line-of-sight operations. Carries MAM-L smart munitions. Used by Turkish Armed Forces and exported to Tunisia and others.",
            specs = "Weight: 1685kg | Range: 200km | Max speed: 218km/h | Endurance: 24h",
            photoAsset = "drones/anka_s.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "rq170_sentinel",
            name = "RQ-170 Sentinel",
            manufacturer = "Lockheed Martin (USA)",
            category = DroneCategory.MILITARY_RECON,
            description = "Stealthy flying-wing ISR drone nicknamed 'Beast of Kandahar.' Used in the Osama bin Laden raid. One captured by Iran in 2011. Very little officially confirmed about its capabilities.",
            specs = "Wingspan: ~20m | Ceiling: >50,000ft | Details classified",
            photoAsset = "drones/rq170_sentinel.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "shahed_238",
            name = "Shahed-238",
            manufacturer = "HESA (Iran)",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Jet-powered evolution of the Shahed-136. Significantly faster and harder to intercept than the propeller-driven 136. Multiple guidance options including IR seeker for anti-ship role.",
            specs = "Weight: ~200kg | Range: 1500km (est.) | Max speed: 600km/h | Warhead: ~40kg",
            photoAsset = "drones/shahed_238.jpg",
            wifiPatterns = emptyList()
        ),

        // ── Loitering Munitions (additional) ──
        DroneReference(
            id = "harpy",
            name = "IAI Harpy",
            manufacturer = "Israel Aerospace Industries",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Anti-radiation loitering munition designed to detect and destroy radar emitters. Launched from a truck-mounted container, it autonomously seeks enemy air defense radars. Exported to China, India, South Korea, and Turkey.",
            specs = "Weight: 135kg | Range: 500km | Max speed: 185km/h | Warhead: 32kg | Loiter: 2.5h",
            photoAsset = "drones/harpy.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "harop",
            name = "IAI Harop",
            manufacturer = "Israel Aerospace Industries",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Advanced man-in-the-loop loitering munition. Unlike Harpy, operator selects the target via datalink. Used by Azerbaijan to devastating effect in 2020 Nagorno-Karabakh war. Can abort and return to loiter.",
            specs = "Weight: 135kg | Range: 1,000km | Max speed: 225km/h | Warhead: 23kg | Loiter: 6h",
            photoAsset = "drones/harop.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "hero_family",
            name = "UVision Hero-30/120/400",
            manufacturer = "UVision (Israel)",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Family of precision loitering munitions in multiple sizes. Hero-30 is man-portable for infantry. Hero-120 is vehicle-launched for medium targets. Hero-400 can destroy tanks. All feature electro-optical guidance.",
            specs = "Weight: 3-40kg | Range: 40-150km | Max speed: 100-185km/h | Warhead: 0.5-10kg",
            photoAsset = "drones/hero_family.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "warmate",
            name = "WB Electronics Warmate",
            manufacturer = "WB Electronics (Poland)",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Lightweight man-portable loitering munition from Poland. Hand-launched by infantry. Used by Ukraine and several NATO allies. Can be recovered if target is not engaged. Simple catapult launch.",
            specs = "Weight: 5.3kg | Range: 30km | Max speed: 150km/h | Warhead: 1.4kg | Loiter: 70min",
            photoAsset = "drones/warmate.jpg",
            wifiPatterns = emptyList()
        ),

        // ── Military Recon (additional) ──
        DroneReference(
            id = "orlan10",
            name = "Orlan-10",
            manufacturer = "Special Technology Center (Russia)",
            category = DroneCategory.MILITARY_RECON,
            description = "Russia's most-used ISR drone in Ukraine, with hundreds lost and captured. Catapult-launched, parachute-recovered. Provides artillery spotting and jamming. Built partly from commercial components including Canon cameras.",
            specs = "Weight: 18kg | Range: 120km | Max speed: 150km/h | Endurance: 16h",
            photoAsset = "drones/orlan10.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "forpost_r",
            name = "Forpost-R",
            manufacturer = "Ural Works (Russia)",
            category = DroneCategory.MILITARY_RECON,
            description = "Russian-built variant of the IAI Searcher II produced under license. Medium-altitude ISR platform. Forpost-R version uses Russian-made components and APD-85 engine to reduce import dependence.",
            specs = "Weight: 500kg | Range: 250km | Max speed: 204km/h | Endurance: 18h",
            photoAsset = "drones/forpost_r.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "ch4",
            name = "CASC CH-4 Rainbow",
            manufacturer = "CASC (China)",
            category = DroneCategory.MILITARY_RECON,
            description = "Chinese medium-altitude ISR and strike drone comparable to MQ-1 Predator. Widely exported to Iraq, Jordan, Saudi Arabia, Algeria, Egypt, and Pakistan as an affordable armed drone option.",
            specs = "Weight: 1330kg | Range: 250km | Max speed: 235km/h | Endurance: 14h",
            photoAsset = "drones/ch4.jpg",
            wifiPatterns = emptyList()
        ),

        // ── Military Strike (additional) ──
        DroneReference(
            id = "ch5",
            name = "CASC CH-5 Rainbow",
            manufacturer = "CASC (China)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Chinese heavy UCAV comparable to MQ-9 Reaper. Can carry up to 16 missiles on 4 hardpoints. Satellite data link for beyond-line-of-sight operations. Exported as cost-effective alternative to Western drones.",
            specs = "Weight: 3300kg | Range: 400km | Max speed: 300km/h | Endurance: 60h | Payload: 1200kg",
            photoAsset = "drones/ch5.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "kargu2",
            name = "STM Kargu-2",
            manufacturer = "STM (Turkey)",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Small autonomous attack quadcopter with AI-powered target recognition. Can operate in swarms. Reported by UN to have autonomously engaged targets in Libya in 2021 — possibly the first autonomous drone strike.",
            specs = "Weight: 7kg | Range: 5km | Max speed: 72km/h | Warhead: 1.4kg | Endurance: 30min",
            photoAsset = "drones/kargu2.jpg",
            wifiPatterns = emptyList()
        ),

        // ── FPV Combat Drones ──
        DroneReference(
            id = "fpv_generic",
            name = "FPV Combat Drone (Generic)",
            manufacturer = "Various / Custom-built",
            category = DroneCategory.FPV_COMBAT,
            description = "Custom-built racing-style quadcopters modified to carry explosives, dominant weapon of the Ukraine war. Typically 5-7 inch frames with analog/digital FPV video. Costs \$400-500 per unit. Pilot flies via goggles into target at 100+ km/h.",
            specs = "Weight: 0.5-2kg | Range: 5-10km | Max speed: 120km/h | Warhead: RPG/grenade",
            photoAsset = "drones/fpv_combat.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "baba_yaga",
            name = "Baba Yaga (Heavy Bomber Drone)",
            manufacturer = "Ukrainian Volunteer Groups",
            category = DroneCategory.FPV_COMBAT,
            description = "Ukrainian large octocopter used for nighttime bomb drops on Russian positions. Named after the Slavic witch. Carries multiple mortar rounds or anti-tank mines. Operates at night using thermal cameras. Terrifying psychological impact.",
            specs = "Weight: 10-20kg | Range: 10-15km | Max speed: 60km/h | Payload: 5-10kg munitions",
            photoAsset = "drones/baba_yaga.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "r18",
            name = "R18 Octocopter",
            manufacturer = "Aerorozvidka (Ukraine)",
            category = DroneCategory.FPV_COMBAT,
            description = "Ukrainian heavy FPV octocopter designed for anti-armor munitions drops. Built by the Aerorozvidka volunteer unit. Drops modified anti-tank grenades with precision onto Russian vehicles and positions from above.",
            specs = "Weight: 12kg | Range: 5km | Max speed: 50km/h | Payload: RKG-3 anti-tank grenades",
            photoAsset = "drones/r18.jpg",
            wifiPatterns = emptyList()
        ),

        // ── Consumer 2024-25 ──
        DroneReference(
            id = "dji_air3s",
            name = "DJI Air 3S",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Updated mid-range foldable drone with improved obstacle avoidance sensors and 4K/60fps HDR video. Dual-camera system with 1/1.3-inch wide and 1/1.3-inch telephoto sensors.",
            specs = "Weight: 720g | Range: 20km | Max speed: 75km/h | Flight time: 46min",
            photoAsset = "drones/dji_air3s.jpg",
            wifiPatterns = listOf("DJI-", "AIR-")
        ),
        DroneReference(
            id = "dji_mini4k",
            name = "DJI Mini 4K",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Ultra-lightweight sub-250g drone with 4K video. No Remote ID required in many jurisdictions due to weight. Ideal entry-level drone for recreational flying.",
            specs = "Weight: 249g | Range: 10km | Max speed: 57km/h | Flight time: 31min",
            photoAsset = "drones/dji_mini4k.jpg",
            wifiPatterns = listOf("DJI-", "MINI-")
        ),
        DroneReference(
            id = "dji_mavic3_classic",
            name = "DJI Mavic 3 Classic",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Cost-reduced Mavic 3 with single Hasselblad camera (no telephoto). Same flight performance and obstacle avoidance as full Mavic 3. 4/3 CMOS sensor.",
            specs = "Weight: 895g | Range: 15km | Max speed: 75km/h | Flight time: 46min",
            photoAsset = "drones/dji_mavic3_classic.jpg",
            wifiPatterns = listOf("DJI-", "MAVIC-")
        ),
        DroneReference(
            id = "dji_avata2",
            name = "DJI Avata 2",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Second-gen FPV cinewhoop with 4K/60fps, 1/1.3-inch sensor. Lighter and more agile than original Avata. Pairs with DJI Goggles 3 for immersive flight. Built-in propeller guards.",
            specs = "Weight: 377g | Range: 13km | Max speed: 108km/h | Flight time: 23min",
            photoAsset = "drones/dji_avata2.jpg",
            wifiPatterns = listOf("DJI-", "AVATA-")
        ),
        DroneReference(
            id = "dji_flip",
            name = "DJI Flip",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Compact sub-250g vlogging drone with flip-up camera for selfies and subject tracking. AI-powered intelligent flight modes for content creators.",
            specs = "Weight: 249g | Range: 13km | Max speed: 57km/h | Flight time: 31min",
            photoAsset = "drones/dji_flip.jpg",
            wifiPatterns = listOf("DJI-", "FLIP-")
        ),
        DroneReference(
            id = "dji_neo",
            name = "DJI Neo",
            manufacturer = "DJI",
            category = DroneCategory.CONSUMER,
            description = "Palm-sized selfie drone that can launch from your hand. AI subject tracking and QuickShots. Ultra-portable for on-the-go content creation. No controller required.",
            specs = "Weight: 135g | Range: 6km | Max speed: 50km/h | Flight time: 18min",
            photoAsset = "drones/dji_neo.jpg",
            wifiPatterns = listOf("DJI-", "NEO-")
        ),
        DroneReference(
            id = "autel_evo3_pro",
            name = "Autel EVO III Pro",
            manufacturer = "Autel",
            category = DroneCategory.CONSUMER,
            description = "Flagship tri-camera drone with 1-inch main sensor, telephoto, and wide-angle. Competes directly with DJI Mavic 3 Pro. Omnidirectional obstacle sensing.",
            specs = "Weight: 900g | Range: 15km | Max speed: 75km/h | Flight time: 42min",
            photoAsset = "drones/autel_evo3_pro.jpg",
            wifiPatterns = listOf("AUTEL-", "EVO-")
        ),
        DroneReference(
            id = "autel_lite_plus",
            name = "Autel EVO Lite+",
            manufacturer = "Autel",
            category = DroneCategory.CONSUMER,
            description = "Mid-range 1-inch CMOS sensor drone with f/1.9 aperture for low-light performance. Moonlight Algorithm for night photography. Three-way obstacle avoidance.",
            specs = "Weight: 835g | Range: 12km | Max speed: 65km/h | Flight time: 40min",
            photoAsset = "drones/autel_lite_plus.jpg",
            wifiPatterns = listOf("AUTEL-", "EVO-")
        ),
        DroneReference(
            id = "skydio_s1",
            name = "Skydio S1",
            manufacturer = "Skydio",
            category = DroneCategory.CONSUMER,
            description = "AI-powered autonomous drone with industry-leading obstacle avoidance using six 4K navigation cameras. Superior autonomous tracking for sports and action. US-manufactured.",
            specs = "Weight: 775g | Range: 6km | Max speed: 58km/h | Flight time: 27min",
            photoAsset = "drones/skydio_s1.jpg",
            wifiPatterns = listOf("SKYDIO-")
        ),
        DroneReference(
            id = "parrot_anafi_ai",
            name = "Parrot Anafi AI",
            manufacturer = "Parrot",
            category = DroneCategory.CONSUMER,
            description = "European-made drone with 4G connectivity for unlimited range. 48MP camera on 4K HDR gimbal. Designed for photogrammetry and inspection. Built-in 4G modem.",
            specs = "Weight: 898g | Range: 4G unlimited | Max speed: 55km/h | Flight time: 32min",
            photoAsset = "drones/parrot_anafi_ai.jpg",
            wifiPatterns = listOf("PARROT-", "ANAFI-")
        ),
        DroneReference(
            id = "hoverair_x1_promax",
            name = "HOVERAir X1 Pro Max",
            manufacturer = "Zero Zero",
            category = DroneCategory.CONSUMER,
            description = "Palm-sized self-flying camera with no controller needed. Gesture and voice control. Upgraded from X1 with longer range and better camera. Foldable prop guards.",
            specs = "Weight: 190g | Range: 100m | Max speed: 35km/h | Flight time: 16min",
            photoAsset = "drones/hoverair_x1_promax.jpg",
            wifiPatterns = listOf("HOVERAIR", "HOVER-")
        ),
        DroneReference(
            id = "potensic_atom_se",
            name = "Potensic ATOM SE",
            manufacturer = "Potensic",
            category = DroneCategory.CONSUMER,
            description = "Budget-friendly sub-250g drone with GPS, 4K camera, and 31-minute flight time. Visual tracking and smart return-to-home. Great value entry-level option.",
            specs = "Weight: 249g | Range: 4km | Max speed: 50km/h | Flight time: 31min",
            photoAsset = "drones/potensic_atom_se.jpg",
            wifiPatterns = listOf("POTENSIC-")
        ),

        // ── Budget / Toy (new) ──
        DroneReference(
            id = "ruko_u11_pro",
            name = "Ruko U11 Pro",
            manufacturer = "Ruko",
            category = DroneCategory.CONSUMER,
            description = "Budget GPS drone with 4K camera and 2-axis gimbal stabilization. Includes carrying case and two batteries. Surprisingly capable for price point.",
            specs = "Weight: 360g | Range: 1.2km | Max speed: 36km/h | Flight time: 26min",
            photoAsset = "drones/ruko_u11_pro.jpg",
            wifiPatterns = listOf("RUKO-")
        ),
        DroneReference(
            id = "simrex_x500",
            name = "SIMREX X500",
            manufacturer = "SIMREX",
            category = DroneCategory.CONSUMER,
            description = "Foldable beginner drone with 4K camera and optical flow positioning. Altitude hold and headless mode for easy learning. Very affordable entry point.",
            specs = "Weight: 200g | Range: 100m | Max speed: 25km/h | Flight time: 15min",
            photoAsset = "drones/simrex_x500.jpg",
            wifiPatterns = listOf("SIMREX-")
        ),
        DroneReference(
            id = "simrex_x900",
            name = "SIMREX X900",
            manufacturer = "SIMREX",
            category = DroneCategory.CONSUMER,
            description = "GPS-enabled budget drone with brushless motors and 4K camera. Follow-me mode and waypoint flight. Step up from X500 with GPS stability.",
            specs = "Weight: 350g | Range: 500m | Max speed: 35km/h | Flight time: 22min",
            photoAsset = "drones/simrex_x900.jpg",
            wifiPatterns = listOf("SIMREX-")
        ),
        DroneReference(
            id = "snaptain_sp510",
            name = "Snaptain SP510",
            manufacturer = "Snaptain",
            category = DroneCategory.CONSUMER,
            description = "Foldable GPS drone with 2.7K camera and brushless motors. Smart return home and follow-me mode. Popular Amazon bestseller in budget category.",
            specs = "Weight: 370g | Range: 500m | Max speed: 30km/h | Flight time: 20min",
            photoAsset = "drones/snaptain_sp510.jpg",
            wifiPatterns = listOf("SNAPTAIN-")
        ),
        DroneReference(
            id = "ryze_tello2",
            name = "Ryze Tello II",
            manufacturer = "Ryze/DJI",
            category = DroneCategory.CONSUMER,
            description = "DJI-powered programmable mini drone for STEM education. Scratch and Python SDK for learning to code with drones. Improved camera and stability over original Tello.",
            specs = "Weight: 87g | Range: 100m | Max speed: 29km/h | Flight time: 13min",
            photoAsset = "drones/ryze_tello2.jpg",
            wifiPatterns = listOf("TELLO-")
        ),
        DroneReference(
            id = "potensic_p5",
            name = "Potensic P5",
            manufacturer = "Potensic",
            category = DroneCategory.CONSUMER,
            description = "Ultra-compact foldable drone with dual cameras and optical flow. One-key takeoff and landing. Popular gift drone for beginners and kids.",
            specs = "Weight: 130g | Range: 100m | Max speed: 25km/h | Flight time: 15min",
            photoAsset = "drones/potensic_p5.jpg",
            wifiPatterns = listOf("POTENSIC-")
        ),
        DroneReference(
            id = "holy_stone_hs175d",
            name = "Holy Stone HS175D",
            manufacturer = "Holy Stone",
            category = DroneCategory.CONSUMER,
            description = "Foldable GPS drone with 2K camera and brushless motors. Smart return home, follow-me, and circle fly. Two batteries included for extended flight.",
            specs = "Weight: 406g | Range: 600m | Max speed: 36km/h | Flight time: 23min",
            photoAsset = "drones/holy_stone_hs175d.jpg",
            wifiPatterns = listOf("HOLY", "HS-")
        ),
        DroneReference(
            id = "holy_stone_hs440",
            name = "Holy Stone HS440",
            manufacturer = "Holy Stone",
            category = DroneCategory.CONSUMER,
            description = "Foldable FPV drone with 1080p camera and voice control. Gesture photo/video and 3D flip tricks. Great indoor/outdoor beginner drone.",
            specs = "Weight: 160g | Range: 100m | Max speed: 25km/h | Flight time: 20min",
            photoAsset = "drones/holy_stone_hs440.jpg",
            wifiPatterns = listOf("HOLY", "HS-")
        ),
        DroneReference(
            id = "neheme_nh525",
            name = "Neheme NH525",
            manufacturer = "Neheme",
            category = DroneCategory.CONSUMER,
            description = "Budget foldable drone with 1080p camera. Altitude hold and headless mode. Popular starter drone on Amazon with multiple color options.",
            specs = "Weight: 100g | Range: 80m | Max speed: 20km/h | Flight time: 12min",
            photoAsset = "drones/neheme_nh525.jpg",
            wifiPatterns = listOf("NEHEME-")
        ),
        DroneReference(
            id = "aovo_v3",
            name = "AOVO V3",
            manufacturer = "AOVO",
            category = DroneCategory.CONSUMER,
            description = "Compact WiFi FPV drone with dual cameras and obstacle avoidance sensors. Budget-friendly with altitude hold and one-key return.",
            specs = "Weight: 150g | Range: 100m | Max speed: 25km/h | Flight time: 15min",
            photoAsset = "drones/aovo_v3.jpg",
            wifiPatterns = listOf("AOVO-")
        ),
        DroneReference(
            id = "4drc_v2",
            name = "4DRC V2 Mini",
            manufacturer = "4DRC",
            category = DroneCategory.CONSUMER,
            description = "Ultra-cheap mini foldable drone with 1080p WiFi camera. 3D flips and headless mode. Very popular low-end Amazon drone for casual use.",
            specs = "Weight: 80g | Range: 80m | Max speed: 20km/h | Flight time: 12min",
            photoAsset = "drones/4drc_v2.jpg",
            wifiPatterns = listOf("4DRC-")
        ),
        DroneReference(
            id = "tenssenx_t80",
            name = "TENSSENX T80",
            manufacturer = "TENSSENX",
            category = DroneCategory.CONSUMER,
            description = "Budget drone with brushless motors and 4K camera. GPS-assisted hover and smart return home. Decent wind resistance for the price.",
            specs = "Weight: 300g | Range: 300m | Max speed: 30km/h | Flight time: 20min",
            photoAsset = "drones/tenssenx_t80.jpg",
            wifiPatterns = listOf("TENSSENX-")
        ),

        // ── Enterprise / Commercial (new) ──
        DroneReference(
            id = "freefly_astro",
            name = "Freefly Astro",
            manufacturer = "Freefly Systems",
            category = DroneCategory.ENTERPRISE,
            description = "Modular mapping and inspection drone with swappable payloads. US-manufactured. Open SDK for custom integration. RTK-capable for survey-grade accuracy.",
            specs = "Weight: 6.8kg | Range: 10km | Max speed: 65km/h | Flight time: 38min",
            photoAsset = "drones/freefly_astro.jpg",
            wifiPatterns = listOf("FREEFLY-")
        ),
        DroneReference(
            id = "sensefly_ebee_x",
            name = "senseFly eBee X",
            manufacturer = "senseFly",
            category = DroneCategory.ENTERPRISE,
            description = "Fixed-wing mapping drone covering up to 500ha in a single flight. Swappable camera payloads including multispectral and thermal. Hand-launched, belly-landing.",
            specs = "Weight: 1.6kg | Range: 10km | Max speed: 110km/h | Flight time: 90min",
            photoAsset = "drones/sensefly_ebee_x.jpg",
            wifiPatterns = listOf("SENSEFLY-")
        ),
        DroneReference(
            id = "dji_dock2",
            name = "DJI Dock 2 / Matrice 3TD",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Autonomous drone-in-a-box system. Matrice 3TD deploys from weatherproof dock for scheduled inspection flights. Triple camera (wide, zoom, thermal). Fully autonomous operations.",
            specs = "Weight: 1920g (drone) | Range: 7km from dock | Max speed: 57km/h | Flight time: 50min",
            photoAsset = "drones/dji_dock2.jpg",
            wifiPatterns = listOf("DJI-", "MATRICE-")
        ),
        DroneReference(
            id = "dji_flycart30",
            name = "DJI FlyCart 30",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Heavy-lift delivery drone carrying up to 30kg payload. Dual battery redundancy and winch delivery system. Designed for cargo delivery to remote areas.",
            specs = "Weight: 42kg (empty) | Payload: 30kg | Max speed: 54km/h | Flight time: 16min (full load)",
            photoAsset = "drones/dji_flycart30.jpg",
            wifiPatterns = listOf("DJI-")
        ),
        DroneReference(
            id = "dji_mavic3e",
            name = "DJI Mavic 3 Enterprise",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Enterprise version of Mavic 3 with mechanical shutter, RTK module, and speaker/spotlight accessories. Thermal variant available. Used for inspection, mapping, and public safety.",
            specs = "Weight: 915g | Range: 15km | Max speed: 75km/h | Flight time: 45min",
            photoAsset = "drones/dji_mavic3e.jpg",
            wifiPatterns = listOf("DJI-", "MAVIC-")
        ),
        DroneReference(
            id = "dji_matrice30t",
            name = "DJI Matrice 30T",
            manufacturer = "DJI",
            category = DroneCategory.ENTERPRISE,
            description = "Ruggedized enterprise drone with IP55 rating and wide-zoom-thermal tri-camera. Foldable for rapid deployment. Pilot 2 remote with built-in screen.",
            specs = "Weight: 3770g | Range: 15km | Max speed: 82km/h | Flight time: 41min",
            photoAsset = "drones/dji_matrice30t.jpg",
            wifiPatterns = listOf("DJI-", "MATRICE-")
        ),
        DroneReference(
            id = "wingcopter_198",
            name = "Wingcopter 198",
            manufacturer = "Wingcopter",
            category = DroneCategory.ENTERPRISE,
            description = "eVTOL fixed-wing delivery drone with patented tilt-rotor mechanism. Transitions from hover to forward flight. Triple-drop delivery system for medical supplies and packages.",
            specs = "Weight: 21kg | Payload: 6kg | Max speed: 150km/h | Range: 110km",
            photoAsset = "drones/wingcopter_198.jpg",
            wifiPatterns = listOf("WINGCOPTER-")
        ),
        DroneReference(
            id = "flyability_elios3",
            name = "Flyability Elios 3",
            manufacturer = "Flyability",
            category = DroneCategory.ENTERPRISE,
            description = "Collision-tolerant indoor inspection drone with protective cage. LiDAR SLAM for GPS-denied 3D mapping. Designed for confined spaces like boilers, tanks, and mines.",
            specs = "Weight: 1950g | Max speed: 25km/h | Flight time: 12min | LiDAR range: 30m",
            photoAsset = "drones/flyability_elios3.jpg",
            wifiPatterns = listOf("FLYABILITY-")
        ),
        DroneReference(
            id = "skydio_dock",
            name = "Skydio Dock",
            manufacturer = "Skydio",
            category = DroneCategory.ENTERPRISE,
            description = "Autonomous drone-in-a-box with Skydio X10. AI-powered autonomous inspection missions. Remote operations via cloud. US-manufactured for government and enterprise.",
            specs = "Weight: 1300g (drone) | Range: 6km from dock | Max speed: 58km/h | Flight time: 35min",
            photoAsset = "drones/skydio_dock.jpg",
            wifiPatterns = listOf("SKYDIO-")
        ),
        DroneReference(
            id = "amazon_mk30",
            name = "Amazon MK30",
            manufacturer = "Amazon Prime Air",
            category = DroneCategory.ENTERPRISE,
            description = "Amazon's latest delivery drone with improved range and rain tolerance. Hexagonal form factor with sense-and-avoid. Delivers packages under 5lb to customers in under an hour.",
            specs = "Weight: ~36kg | Payload: 2.3kg | Max speed: 80km/h | Range: 12km",
            photoAsset = "drones/amazon_mk30.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "joby_s4",
            name = "Joby S4 eVTOL",
            manufacturer = "Joby Aviation",
            category = DroneCategory.ENTERPRISE,
            description = "Electric air taxi with 6 tilting propellers for vertical takeoff and wing-borne cruise. Piloted aircraft designed for urban air mobility. FAA certification in progress.",
            specs = "Weight: 1815kg | Passengers: 4 | Max speed: 321km/h | Range: 161km",
            photoAsset = "drones/joby_s4.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "impossible_us1",
            name = "Impossible Aerospace US-1",
            manufacturer = "Impossible Aerospace",
            category = DroneCategory.ENTERPRISE,
            description = "Long-endurance quadcopter with battery-as-airframe design. 78-minute flight time. Built in the US for public safety and defense. Thermal and zoom payloads available.",
            specs = "Weight: 6.8kg | Range: 5km | Max speed: 67km/h | Flight time: 78min",
            photoAsset = "drones/impossible_us1.jpg",
            wifiPatterns = emptyList()
        ),

        // ── Military / Defense (new) ──
        DroneReference(
            id = "switchblade_300",
            name = "Switchblade 300",
            manufacturer = "AeroVironment",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Man-portable loitering munition launched from a tube. Backpackable by a single soldier. GPS and EO guidance with operator-in-the-loop. Extensively supplied to Ukraine.",
            specs = "Weight: 2.5kg | Range: 10km | Max speed: 160km/h | Loiter: 15min | Warhead: anti-personnel",
            photoAsset = "drones/switchblade_300.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "shield_ai_vbat",
            name = "Shield AI V-BAT",
            manufacturer = "Shield AI",
            category = DroneCategory.MILITARY_RECON,
            description = "Autonomous VTOL fixed-wing UAS that needs no GPS or comms link. AI pilot 'Hivemind' enables fully autonomous ISR missions. Tail-sitter design launches and lands vertically.",
            specs = "Weight: 25kg | Range: 100km | Max speed: 167km/h | Endurance: 9h | Payload: 3.6kg",
            photoAsset = "drones/shield_ai_vbat.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "ghost_mkii",
            name = "Anduril Ghost MkII",
            manufacturer = "Anduril",
            category = DroneCategory.MILITARY_RECON,
            description = "AI-powered multi-mission sUAS from Anduril. Autonomous navigation, target recognition, and sensor fusion via Lattice OS. Used by US special operations forces.",
            specs = "Weight: 6.8kg | Range: 5km | Max speed: 65km/h | Endurance: 100min",
            photoAsset = "drones/ghost_mkii.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "coyote_block3",
            name = "Raytheon Coyote Block 3+",
            manufacturer = "Raytheon",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Tube-launched counter-UAS and strike drone. Block 3+ features warhead for kinetic defeat of enemy drones. Part of the HOWLER and KuRFS C-UAS systems.",
            specs = "Weight: 5.9kg | Range: 10km | Max speed: 130km/h | Endurance: 60min",
            photoAsset = "drones/coyote_block3.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "altius_600",
            name = "Anduril ALTIUS-600",
            manufacturer = "Anduril",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Modular, tube-launched loitering munition/ISR drone. Air-launched from C-130 or ground-launched. Reconfigurable payloads including EW, ISR, and kinetic warhead.",
            specs = "Weight: 12kg | Range: 90km | Max speed: 175km/h | Endurance: 4h",
            photoAsset = "drones/altius_600.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "rq21_blackjack",
            name = "RQ-21A Blackjack",
            manufacturer = "Insitu/Boeing",
            category = DroneCategory.MILITARY_RECON,
            description = "Ship-launched small tactical UAS used by USMC and Navy. Catapult-launched and SkyHook recovered (no runway needed). Multi-INT payload with EO/IR and SIGINT.",
            specs = "Weight: 61kg | Range: 93km | Max speed: 167km/h | Endurance: 16h",
            photoAsset = "drones/rq21_blackjack.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "rq7_shadow",
            name = "RQ-7 Shadow",
            manufacturer = "Textron",
            category = DroneCategory.MILITARY_RECON,
            description = "US Army's primary tactical UAS for brigade-level ISR. Catapult-launched, arresting-gear recovered. Over 1 million flight hours. EO/IR and laser designator payloads.",
            specs = "Weight: 170kg | Range: 125km | Max speed: 204km/h | Endurance: 9h",
            photoAsset = "drones/rq7_shadow.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "scan_eagle",
            name = "ScanEagle",
            manufacturer = "Insitu/Boeing",
            category = DroneCategory.MILITARY_RECON,
            description = "Long-endurance small UAS for maritime and land ISR. Launched via catapult, recovered with SkyHook wire system. Widely used by US Navy and allies. Over 1.5 million flight hours.",
            specs = "Weight: 22kg | Range: 100km | Max speed: 148km/h | Endurance: 28h",
            photoAsset = "drones/scan_eagle.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "puma_ae",
            name = "RQ-20B Puma AE",
            manufacturer = "AeroVironment",
            category = DroneCategory.MILITARY_RECON,
            description = "Hand-launched small UAS for squad-level ISR. All-environment (AE) version with waterproof design for maritime operations. EO/IR gimballed payload.",
            specs = "Weight: 6.3kg | Range: 20km | Max speed: 83km/h | Endurance: 3.5h",
            photoAsset = "drones/puma_ae.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "mq28_ghost_bat",
            name = "MQ-28 Ghost Bat",
            manufacturer = "Boeing Australia",
            category = DroneCategory.MILITARY_STRIKE,
            description = "AI-powered autonomous loyal wingman developed for Royal Australian Air Force. Accompanies manned fighters as force multiplier. First combat aircraft designed and built in Australia in 50 years.",
            specs = "Weight: ~6000kg | Range: 3700km | Max speed: subsonic | Payload: modular nose",
            photoAsset = "drones/mq28_ghost_bat.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "phoenix_ghost",
            name = "Phoenix Ghost",
            manufacturer = "Aevex Aerospace",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Loitering munition developed rapidly for Ukraine under USAF Big Safari program. Details largely classified. Believed to have 6+ hour loiter time and anti-armor capability.",
            specs = "Weight: classified | Range: classified | Endurance: 6h+ | Warhead: anti-armor",
            photoAsset = "drones/phoenix_ghost.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "kratos_xq67a",
            name = "Kratos XQ-67A",
            manufacturer = "Kratos",
            category = DroneCategory.MILITARY_STRIKE,
            description = "Off-board sensing station (OBSS) autonomous jet drone. Part of USAF's Collaborative Combat Aircraft (CCA) program. Designed as affordable attritable wingman for manned fighters.",
            specs = "Weight: ~2700kg | Range: 3000km+ | Max speed: subsonic | Payload: modular",
            photoAsset = "drones/kratos_xq67a.jpg",
            wifiPatterns = emptyList()
        ),
        DroneReference(
            id = "harop_ng",
            name = "IAI Harop NG",
            manufacturer = "IAI",
            category = DroneCategory.LOITERING_MUNITION,
            description = "Next-generation Harop with extended range, improved AI target recognition, and enhanced datalink. Builds on combat-proven Harop used by Azerbaijan, India, and others.",
            specs = "Weight: ~135kg | Range: 1,000km+ | Max speed: 225km/h | Warhead: 23kg | Loiter: 9h",
            photoAsset = "drones/harop_ng.jpg",
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
