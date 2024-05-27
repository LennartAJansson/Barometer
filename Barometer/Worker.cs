namespace Barometer;

using System.Device.I2c;

using Barometer.Data;
using Barometer.Model;

using Iot.Device.Bmxx80;
using Iot.Device.Bmxx80.PowerMode;

public class Worker(ILogger<Worker> logger, IServiceScopeFactory factory)
  : BackgroundService
{
  protected override async Task ExecuteAsync(CancellationToken stoppingToken)
  {
    using IServiceScope scope = factory.CreateScope();
    BarometerContext context = scope.ServiceProvider.GetRequiredService<BarometerContext>();
    I2cConnectionSettings i2cSettings = new(1, 0x76);
    using I2cDevice i2cDevice = I2cDevice.Create(i2cSettings);
    using Bme280 bme280 = new(i2cDevice);
    int measurementTime = bme280.GetMeasurementDuration();

    while (!stoppingToken.IsCancellationRequested)
    {
      bme280.SetPowerMode(Bmx280PowerMode.Forced);
      await Task.Delay(measurementTime, stoppingToken);
      _ = bme280.TryReadTemperature(out UnitsNet.Temperature temperatureValue);
      _ = bme280.TryReadPressure(out UnitsNet.Pressure pressureValue);
      _ = bme280.TryReadHumidity(out UnitsNet.RelativeHumidity humidityValue);
      _ = bme280.TryReadAltitude(out UnitsNet.Length altitudeValue);

      if (logger.IsEnabled(LogLevel.Information))
      {
        logger.LogInformation("Worker running at: {time}, " +
          "temperature: {t}, pressure: {p}, humidity: {h}, altitude: {a}",
          DateTimeOffset.Now,
          temperatureValue,
          pressureValue,
          humidityValue,
          altitudeValue);
        Measure value = new()
        {
          Temperature = temperatureValue.DegreesCelsius,
          Pressure = pressureValue.Hectopascals,
          Humidity = humidityValue.Value,
          Altitude = altitudeValue.Value
        };
        _ = context.Add(value);
        _ = await context.SaveChangesAsync(stoppingToken);
      }
      await Task.Delay(TimeSpan.FromMinutes(15), stoppingToken);
    }
  }
}
