using System;
using System.IO;
using Xunit;

namespace Jellyfin.Plugin.CensorCut.Tests;

/// <summary>
/// Tests for reading the edit list written by the CensorCut desktop app.
/// </summary>
public class EditListTests : IDisposable
{
    // Byte-for-byte the shape src/core/EditList.cpp emits. If the writer's
    // field names ever drift from the reader's, these tests are what catches
    // it — the failure mode otherwise is a silently uncensored film.
    private const string SampleJson = """
    {
      "schemaVersion": 1,
      "generator": "censorcut",
      "sourceFileName": "Example.mkv",
      "sourceHash": "deadbeef",
      "durationMs": 600000,
      "defaultProfileId": "age-7",
      "profiles": [
        {
          "id": "age-5",
          "label": "Age 5",
          "minAge": 5,
          "leadInMs": 150,
          "cuts": [
            { "startMs": 10000, "endMs": 20000, "category": "Violence" },
            { "startMs": 50000, "endMs": 60000 }
          ]
        },
        {
          "id": "age-7",
          "label": "Age 7",
          "minAge": 7,
          "leadInMs": 200,
          "cuts": [
            { "startMs": 10000, "endMs": 20000 }
          ]
        }
      ]
    }
    """;

    private readonly string _dir;

    /// <summary>
    /// Initializes a new instance of the <see cref="EditListTests"/> class.
    /// </summary>
    public EditListTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "censorcut-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    /// <inheritdoc />
    public void Dispose()
    {
        try
        {
            Directory.Delete(_dir, true);
        }
        catch (IOException)
        {
            // Best effort; a leftover temp dir is harmless.
        }

        GC.SuppressFinalize(this);
    }

    private string WriteSample(string json = SampleJson)
    {
        var movie = Path.Combine(_dir, "Example.mkv");
        File.WriteAllText(EditList.PathFor(movie), json);
        return movie;
    }

    [Fact]
    public void PathForSitsBesideTheMovie()
    {
        Assert.Equal(
            Path.Combine("films", "Example.mkv.censorcut-edl.json"),
            EditList.PathFor(Path.Combine("films", "Example.mkv")));
    }

    [Fact]
    public void ParsesEveryFieldTheWriterEmits()
    {
        var list = EditList.LoadFor(WriteSample());

        Assert.NotNull(list);
        Assert.Equal(1, list!.SchemaVersion);
        Assert.Equal("Example.mkv", list.SourceFileName);
        Assert.Equal("deadbeef", list.SourceHash);
        Assert.Equal(600000, list.DurationMs);
        Assert.Equal("age-7", list.DefaultProfileId);
        Assert.Equal(2, list.Profiles.Count);

        var young = list.Profiles[0];
        Assert.Equal("age-5", young.Id);
        Assert.Equal("Age 5", young.Label);
        Assert.Equal(5, young.MinAge);
        Assert.Equal(150, young.LeadInMs);
        Assert.Equal(2, young.Cuts.Count);
        Assert.Equal(10000, young.Cuts[0].StartMs);
        Assert.Equal(20000, young.Cuts[0].EndMs);
        Assert.Equal("Violence", young.Cuts[0].Category);
        Assert.Null(young.Cuts[1].Category);
    }

    [Fact]
    public void MissingEditListIsNotAnError()
    {
        Assert.Null(EditList.LoadFor(Path.Combine(_dir, "NoSuchMovie.mkv")));
        Assert.Null(EditList.LoadFor(null));
        Assert.Null(EditList.LoadFor(string.Empty));
    }

    [Fact]
    public void MalformedEditListLoadsAsNullRatherThanThrowing()
    {
        Assert.Null(EditList.LoadFor(WriteSample("{ this is not json")));
    }

    [Fact]
    public void ConfiguredProfileWins()
    {
        var list = EditList.LoadFor(WriteSample())!;
        Assert.Equal("age-5", list.PickProfile("age-5")!.Id);
    }

    [Fact]
    public void FileDefaultUsedWhenNothingConfigured()
    {
        var list = EditList.LoadFor(WriteSample())!;
        Assert.Equal("age-7", list.PickProfile(null)!.Id);
        Assert.Equal("age-7", list.PickProfile(string.Empty)!.Id);
    }

    [Fact]
    public void UnknownProfileFallsBackToStrictestNotToNone()
    {
        // The dangerous failure here is returning nothing and playing the film
        // uncut, so an unrecognised id must land on the youngest profile.
        var list = EditList.LoadFor(WriteSample())!;
        var picked = list.PickProfile("age-does-not-exist");

        Assert.NotNull(picked);
        Assert.Equal("age-5", picked!.Id);
    }

    [Fact]
    public void EmptyProfileListYieldsNoProfile()
    {
        var json = """
        { "schemaVersion": 1, "defaultProfileId": "age-7", "profiles": [] }
        """;
        var list = EditList.LoadFor(WriteSample(json))!;

        Assert.NotNull(list);
        Assert.Null(list.PickProfile(null));
    }
}
