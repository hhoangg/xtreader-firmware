#include "ManifestPager.h"

ManifestPager::ManifestPager(uint32_t maxPages) : maxPages_(maxPages) {}

bool ManifestPager::onPageTrailer(const ManifestTrailer& trailer) {
  pagesFetched_++;

  if (!trailer.hasNextCursor || trailer.nextCursor.empty()) {
    nextCursor_.clear();
    return false;  // server says this was the last page
  }
  if (pagesFetched_ >= maxPages_) {
    exceededPageLimit_ = true;
    nextCursor_.clear();
    return false;  // bounded: refuse to keep following cursors forever
  }
  nextCursor_ = trailer.nextCursor;
  return true;
}
