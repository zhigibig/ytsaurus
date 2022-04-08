package fetcher

import (
	"context"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"sync"
	"time"

	"github.com/google/pprof/profile"

	"a.yandex-team.ru/library/go/core/log"
	logzap "a.yandex-team.ru/library/go/core/log/zap"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/ytprof"
	"a.yandex-team.ru/yt/go/ytprof/internal/storage"
)

type (
	Fetcher struct {
		config      Config
		yc          yt.Client
		tableYTPath ypath.Path

		services []ServiceFetcher

		l *logzap.Logger
	}

	ServiceFetcher struct {
		service Service
		f       *Fetcher

		resolvers []ResolverFetcher
	}

	ResolverFetcher struct {
		resolver Resolver
		sf       *ServiceFetcher
	}
)

func NewFetcher(yc yt.Client, config Config, l *logzap.Logger) *Fetcher {
	f := new(Fetcher)
	f.yc = yc
	f.config = config
	f.l = l
	f.tableYTPath = ypath.Path(config.TablePath)

	f.services = make([]ServiceFetcher, len(config.Services))
	for id, service := range config.Services {
		f.services[id] = *NewServiceFetcher(service, f)
	}

	return f
}

func NewServiceFetcher(service Service, f *Fetcher) *ServiceFetcher {
	sf := new(ServiceFetcher)
	sf.service = service
	sf.f = f

	sf.resolvers = make([]ResolverFetcher, len(service.Resolvers))
	for id, resolver := range service.Resolvers {
		sf.resolvers[id] = *NewResolverFetcher(resolver, sf)
	}

	return sf
}

func NewResolverFetcher(resolver Resolver, sf *ServiceFetcher) *ResolverFetcher {
	rf := new(ResolverFetcher)
	rf.resolver = resolver
	rf.sf = sf

	return rf
}

func (f *Fetcher) RunFetcherContinious() error {
	for _, service := range f.services {
		go service.runServiceFetcherContinious()
	}

	select {}
}

func (sf *ServiceFetcher) runServiceFetcherContinious() {
	err := ytprof.MigrateTables(sf.f.yc, sf.f.tableYTPath)

	if err != nil {
		sf.f.l.Fatal("migraton failed", log.Error(err), log.String("table_path", sf.f.tableYTPath.String()))
		return
	}

	sf.f.l.Debug("migraton succeded", log.String("table_path", sf.f.tableYTPath.String()))

	for {
		go sf.fetchService()

		time.Sleep(sf.service.Period)
	}
}

func (sf *ServiceFetcher) fetchService() {
	sz := len(sf.resolvers)

	results := make([][]*profile.Profile, sz)
	resultslice := make([]*profile.Profile, 0)
	errs := make([][]error, sz)

	sf.f.l.Debug("all corutines getting started", log.String("profile_service", sf.service.ProfilePath))

	var wg sync.WaitGroup
	wg.Add(sz)
	for id, resolver := range sf.resolvers {
		go func(id int, resolver ResolverFetcher) {
			defer wg.Done()
			results[id], errs[id] = resolver.fetchResolver()
		}(id, resolver)
	}

	sf.f.l.Debug("all corutines started", log.String("profile_service", sf.service.ProfilePath))

	wg.Wait()

	sf.f.l.Debug("all corutines finished", log.String("profile_service", sf.service.ProfilePath))

	for i := 0; i < len(results); i++ {
		for j := 0; j < len(results[i]); j++ {
			if len(errs) <= i || len(errs[i]) <= j {
				sf.f.l.Error("error while running fetch service (errs and results size don't match)")
				continue
			}

			if errs[i][j] != nil {
				sf.f.l.Error("error while running fetch service", log.String("cluster", sf.f.config.Cluster), log.Error(errs[i][j]))
				continue
			}

			resultslice = append(resultslice, results[i][j])
		}
	}

	sf.f.l.Debug("getting ready to push data", log.Int("data_size", len(resultslice)))
	storage := storage.NewTableStorage(sf.f.yc, sf.f.tableYTPath, sf.f.l)
	err := storage.PushData(resultslice, context.Background())
	if err != nil {
		sf.f.l.Error("error while storing profiles", log.String("cluster", sf.f.config.Cluster), log.Error(err))
	}
}

func (rf *ResolverFetcher) fetchResolver() ([]*profile.Profile, []error) {
	var usedIDs []int

	rand.Float64()
	for i := 0; i < len(rf.resolver.Urls); i++ {
		result := rand.Float64()
		if result < rf.sf.service.Probability {
			usedIDs = append(usedIDs, i)
		}
	}

	errs := make([]error, len(usedIDs))
	results := make([]*profile.Profile, len(usedIDs))

	var wg sync.WaitGroup
	wg.Add(len(usedIDs))
	for i := 0; i < len(usedIDs); i++ {
		go func(id int) {
			defer wg.Done()
			results[id], errs[id] = rf.fetchURL(usedIDs[id])
		}(i)
	}

	wg.Wait()

	rf.sf.f.l.Debug("service resolver finished", log.String("service", rf.sf.service.ServiceType), log.Int("profiles_fetched", len(results)))
	return results, errs
}

func (rf *ResolverFetcher) fetchURL(id int) (*profile.Profile, error) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	requestText := fmt.Sprintf("%v:%d/%v", rf.resolver.Urls[id], rf.resolver.Port, rf.sf.service.ProfilePath)
	rf.sf.f.l.Debug("sending request", log.String("request_text", requestText))
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, requestText, nil)
	if err != nil {
		return nil, err
	}

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}

	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("not ok response stasus (%v)", resp.Status)
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}

	return profile.ParseData(body)
}
