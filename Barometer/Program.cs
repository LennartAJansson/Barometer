using Barometer;
using Barometer.Data;

using Microsoft.EntityFrameworkCore;
//https://learn.microsoft.com/en-us/dotnet/iot/deployment#deploying-a-self-contained-app
//https://learn.microsoft.com/en-us/dotnet/iot/tutorials/temp-sensor
//https://learn.microsoft.com/en-us/dotnet/api/system.device.i2c.i2cdevice?view=iot-dotnet-latest
//https://learn.microsoft.com/en-us/dotnet/api/iot.device.bmxx80.bme280?view=iot-dotnet-latest
//https://raspberrypi.stackexchange.com/questions/143609/bmp280-level-5-error

HostApplicationBuilder builder = Host.CreateApplicationBuilder(args);
string? connStr = builder.Configuration.GetConnectionString("DefaultConnection");
builder.Services.AddDbContext<BarometerContext>(options =>
  options.UseMySql(connStr, ServerVersion.AutoDetect(connStr)));
builder.Services.AddHostedService<Worker>();

IHost host = builder.Build();
host.Run();
