package com.friendorfoe.di

import com.friendorfoe.BuildConfig
import com.friendorfoe.data.remote.AdsbFiApiService
import com.friendorfoe.data.remote.AirplanesLiveApiService
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
    @Named("opensky")
    fun provideOpenSkyRetrofit(okHttpClient: OkHttpClient): Retrofit {
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
    fun provideAdsbFiRetrofit(okHttpClient: OkHttpClient): Retrofit {
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
    fun provideAirplanesLiveRetrofit(okHttpClient: OkHttpClient): Retrofit {
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
