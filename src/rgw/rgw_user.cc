// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include "rgw_sal_rados.h"

#include "include/types.h"
#include "rgw_user.h"

// until everything is moved from rgw_common
#include "rgw_common.h"

#define dout_subsys ceph_subsys_rgw

using namespace std;

int rgw_sync_all_stats(const DoutPrefixProvider *dpp,
                       optional_yield y, rgw::sal::Driver* driver,
                       const rgw_owner& owner, const std::string& tenant)
{
  size_t max_entries = dpp->get_cct()->_conf->rgw_list_buckets_max_chunk;

  rgw::sal::BucketList listing;
  int ret = 0;
  do {
    ret = driver->list_buckets(dpp, owner, tenant, listing.next_marker,
                               string(), max_entries, false, listing, y);
    if (ret < 0) {
      ldpp_dout(dpp, 0) << "failed to list buckets: " << cpp_strerror(ret) << dendl;
      return ret;
    }

    for (auto& ent : listing.buckets) {
      std::unique_ptr<rgw::sal::Bucket> bucket;
      ret = driver->load_bucket(dpp, ent.bucket, &bucket, y);
      if (ret < 0) {
        ldpp_dout(dpp, 0) << "ERROR: could not read bucket info: bucket=" << bucket << " ret=" << ret << dendl;
        continue;
      }
      ret = bucket->sync_owner_stats(dpp, y, &ent);
      if (ret < 0) {
        ldpp_dout(dpp, 0) << "ERROR: could not sync bucket stats: ret=" << ret << dendl;
        return ret;
      }
      ret = bucket->check_bucket_shards(dpp, ent.count, y);
      if (ret < 0) {
	ldpp_dout(dpp, 0) << "ERROR in check_bucket_shards: " << cpp_strerror(-ret)<< dendl;
      }
    }
  } while (!listing.next_marker.empty());

  ret = driver->complete_flush_stats(dpp, y, owner);
  if (ret < 0) {
    ldpp_dout(dpp, 0) << "ERROR: failed to complete syncing owner stats: ret=" << ret << dendl;
    return ret;
  }

  return 0;
}

int rgw_user_get_all_buckets_stats(const DoutPrefixProvider *dpp,
				   rgw::sal::Driver* driver,
				   rgw::sal::User* user,
				   map<string, bucket_meta_entry>& buckets_usage_map,
				   optional_yield y)
{
  size_t max_entries = dpp->get_cct()->_conf->rgw_list_buckets_max_chunk;

  rgw::sal::BucketList listing;
  do {
    int ret = driver->list_buckets(dpp, user->get_id(), user->get_tenant(),
                                   listing.next_marker, string(),
                                   max_entries, false, listing, y);
    if (ret < 0) {
      ldpp_dout(dpp, 0) << "failed to read user buckets: ret=" << ret << dendl;
      return ret;
    }

    for (const auto& ent : listing.buckets) {
      bucket_meta_entry entry;
      entry.size = ent.size;
      entry.size_rounded = ent.size_rounded;
      entry.creation_time = ent.creation_time;
      entry.count = ent.count;
      buckets_usage_map.emplace(ent.bucket.name, entry);
    }
  } while (!listing.next_marker.empty());

  return 0;
}

int rgw_validate_tenant_name(const string& t)
{
  struct tench {
    static bool is_good(char ch) {
      return isalnum(ch) || ch == '_';
    }
  };
  std::string::const_iterator it =
    std::find_if_not(t.begin(), t.end(), tench::is_good);
  return (it == t.end())? 0: -ERR_INVALID_TENANT_NAME;
}

/**
 * Get the anonymous (ie, unauthenticated) user info.
 */
void rgw_get_anon_user(RGWUserInfo& info)
{
  info.user_id = RGW_USER_ANON_ID;
  info.display_name.clear();
  info.access_keys.clear();
}

static bool char_is_unreserved_url(char c)
{
  if (isalnum(c))
    return true;

  switch (c) {
    case '-':
    case '.':
    case '_':
    case '~':
      return true;
    default:
      return false;
  }
}

static bool validate_access_key(string& key)
{
  const char *p = key.c_str();
  while (*p) {
    if (!char_is_unreserved_url(*p))
      return false;
    p++;
  }
  return true;
}

int rgw_generate_access_key(const DoutPrefixProvider* dpp,
                            optional_yield y,
                            rgw::sal::Driver* driver,
                            std::string& access_key_id)
{
  std::string id;
  int r = 0;

  do {
    id.resize(PUBLIC_ID_LEN + 1);
    gen_rand_alphanumeric_upper(dpp->get_cct(), id.data(), id.size());
    id.pop_back(); // remove trailing null

    if (!validate_access_key(id))
      continue;

    std::unique_ptr<rgw::sal::User> duplicate_check;
    r = driver->get_user_by_access_key(dpp, id, y, &duplicate_check);
  } while (r == 0);

  if (r == -ENOENT) {
    access_key_id = std::move(id);
    return 0;
  }
  return r;
}

void rgw_generate_secret_key(CephContext* cct,
                             std::string& secret_key)
{
  char secret_key_buf[SECRET_KEY_LEN + 1];
  gen_rand_alphanumeric_plain(cct, secret_key_buf, sizeof(secret_key_buf));
  secret_key = secret_key_buf;
}

