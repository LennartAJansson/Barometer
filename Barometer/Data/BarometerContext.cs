namespace Barometer.Data;

using System.Reflection;

using Barometer.Model;

using Microsoft.EntityFrameworkCore;

internal class BarometerContext(DbContextOptions<BarometerContext> options) 
  : DbContext(options)
{
  public DbSet<Measure> Measures { get; set; }

  protected override void OnModelCreating(ModelBuilder modelBuilder)
  {
    modelBuilder
      .ApplyConfigurationsFromAssembly(Assembly.GetExecutingAssembly());
  }
}
