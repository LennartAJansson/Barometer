namespace Barometer.Model;
using System;

internal class Measure
{
  public int Id { get; set; }
  public double Temperature { get; set; }
  public double Pressure { get; set; }
  public double Humidity { get; set; }
  public double Altitude { get; set; }
  public DateTimeOffset Registered { get; set; } = DateTimeOffset.Now;
}
