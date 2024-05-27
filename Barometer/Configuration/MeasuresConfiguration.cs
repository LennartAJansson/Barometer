namespace Barometer.Configuration;

using Barometer.Model;

using Microsoft.EntityFrameworkCore;
using Microsoft.EntityFrameworkCore.Metadata.Builders;

internal class MeasuresConfiguration
  : IEntityTypeConfiguration<Measure>
{
  public void Configure(EntityTypeBuilder<Measure> builder) => builder.ToTable("Measures");
}
