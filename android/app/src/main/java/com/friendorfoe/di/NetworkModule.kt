package com.friendorfoe.di

import com.friendorfoe.BuildConfig
import com.friendorfoe.data.remote.AdsbFiApiService
import com.friendorfoe.data.remote.AdsbLolApiService
import com.friendorfoe.data.remote.AdsbOneApiService
import com.friendorfoe.data.remote.AirplanesLiveApiService
import com.friendorfoe.data.remote.HexDbApiService
import com.friendorfoe.data.remote.OpenMeteoApiService
import com.friendorfoe.data.remote.OpenSkyApiService
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory
import java.util.concurrent.TimeUnit
import javax.inject.Named
import javax.inject.Singleton

/**
 * Hilt module providing networking dependencies (OkHttp, Retrofit, API service).
 */
@Module
@InstallIn(SingletonComponent::class)
object NetworkModule {

    private const val OPENSKY_BASE_URL = "https://opensky-network.org/api/"
    private const val ADSBFI_BASE_URL = "https://opendata.adsb.fi/api/"
    private const val AIRPLANES_LIVE_BASE_URL = "https://api.airplanes.live/"
    private const val ADSB_LOL_BASE_URL = "https://api.adsb.lol/"
    private const val ADSB_ONE_BASE_URL = "https://api.adsb.one/"
    private const val HEXDB_BASE_URL = "https://hexdb.io/"
    private const val OPEN_METEO_BASE_URL = "https://api.open-meteo.com/"

    @Provides
    @Singleton
    fun provideLoggingInterceptor(): HttpLoggingInterceptor {
        return HttpLoggingInterceptor().apply {
            level = if (BuildConfig.DEBUG) HttpLoggingInterceptor.Level.BASIC
                    else HttpLoggingInterceptor.Level.NONE
        }
    }

    @Provides
    @Singleton
    fun provideOkHttpClient(
        loggingInterceptor: HttpLoggingInterceptor
    ): OkHttpClient {
        return OkHttpClient.Builder()
            .addInterceptor(loggingInterceptor)
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .writeTimeout(15, TimeUnit.SECONDS)
            .build()
    }

    @Provides
    @Singleton
    @Named("adsb")
    fun provideAdsbOkHttpClient(
        loggingInterceptor: HttpLoggingInterceptor
    ): OkHttpClient {
        return OkHttpClient.Builder()
            .addInterceptor(loggingInterceptor)
            .connectTimeout(8, TimeUnit.SECONDS)
            .readTimeout(8, TimeUnit.SECONDS)
            .writeTimeout(8, TimeUnit.SECONDS)
            .build()
    }

    @Provides
    @Singleton
    @Named("opensky")
    fun provideOpenSkyRetrofit(@Named("adsb") okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(OPENSKY_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideOpenSkyApiService(@Named("opensky") retrofit: Retrofit): OpenSkyApiService {
        return retrofit.create(OpenSkyApiService::class.java)
    }

    @Provides
    @Singleton
    @Named("adsbfi")
    fun provideAdsbFiRetrofit(@Named("adsb") okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(ADSBFI_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideAdsbFiApiService(@Named("adsbfi") retrofit: Retrofit): AdsbFiApiService {
        return retrofit.create(AdsbFiApiService::class.java)
    }

    @Provides
    @Singleton
    @Named("airplaneslive")
    fun provideAirplanesLiveRetrofit(@Named("adsb") okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(AIRPLANES_LIVE_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideAirplanesLiveApiService(@Named("airplaneslive") retrofit: Retrofit): AirplanesLiveApiService {
        return retrofit.create(AirplanesLiveApiService::class.java)
    }

    @Provides
    @Singleton
    @Named("adsblol")
    fun provideAdsbLolRetrofit(@Named("adsb") okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(ADSB_LOL_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideAdsbLolApiService(@Named("adsblol") retrofit: Retrofit): AdsbLolApiService {
        return retrofit.create(AdsbLolApiService::class.java)
    }

    @Provides
    @Singleton
    @Named("adsbone")
    fun provideAdsbOneRetrofit(@Named("adsb") okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(ADSB_ONE_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideAdsbOneApiService(@Named("adsbone") retrofit: Retrofit): AdsbOneApiService {
        return retrofit.create(AdsbOneApiService::class.java)
    }

    @Provides
    @Singleton
    @Named("hexdb")
    fun provideHexDbRetrofit(okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(HEXDB_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideHexDbApiService(@Named("hexdb") retrofit: Retrofit): HexDbApiService {
        return retrofit.create(HexDbApiService::class.java)
    }

    @Provides
    @Singleton
    @Named("openmeteo")
    fun provideOpenMeteoRetrofit(okHttpClient: OkHttpClient): Retrofit {
        return Retrofit.Builder()
            .baseUrl(OPEN_METEO_BASE_URL)
            .client(okHttpClient)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }

    @Provides
    @Singleton
    fun provideOpenMeteoApiService(@Named("openmeteo") retrofit: Retrofit): OpenMeteoApiService {
        return retrofit.create(OpenMeteoApiService::class.java)
    }
}
