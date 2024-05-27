using Barometer;
using Barometer.Data;

using Microsoft.EntityFrameworkCore;

HostApplicationBuilder builder = Host.CreateApplicationBuilder(args);
string? connStr = builder.Configuration.GetConnectionString("DefaultConnection");
builder.Services.AddDbContext<BarometerContext>(options =>
  options.UseMySql(connStr, ServerVersion.AutoDetect(connStr)));
builder.Services.AddHostedService<Worker>();

IHost host = builder.Build();
host.Run();
